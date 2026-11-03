/* Software renderer for WordCraft's Open Watcom host. MIT OR Apache-2.0. */
#include "raster.h"

#include <stdlib.h>
#include <string.h>

#define MAX_SIDE 8192U
#define MAX_BYTES (64UL * 1024UL * 1024UL)
#define MAX_TEXTURES 512U
#define MAX_MESHES 8192U
#define MAX_VERTICES 1048576U
#define MAX_INDICES (3U * MAX_VERTICES)
#define SUBPIXEL 256
#define HALF_PIXEL (SUBPIXEL / 2)

typedef struct {
    uint8_t b, g, r, a;
} Bgra;

typedef struct {
    uint32_t id, width, height;
    /* Packed sRGBA, with red in the least significant byte. */
    uint32_t *pixels;
} Texture;

typedef struct {
    unsigned slot;
    uint32_t id, x, y, width, height;
    const uint8_t *pixels;
    uint32_t *replacement;
} Upload;

typedef struct {
    int32_t x, y;
    float u, v;
    uint32_t color;
} Vertex;

typedef struct {
    int32_t left, top, right, bottom;
} Clip;

typedef struct {
    uint32_t id, vertex_count, index_count;
    float clip[4];
    const uint8_t *vertices, *indices;
} Mesh;

typedef struct {
    const uint8_t *next;
    size_t remaining;
} Reader;

typedef struct {
    int64_t value, dx, dy;
} Edge;

typedef struct {
    double start, dx, dy;
} Plane;

struct WcRaster {
    Texture textures[MAX_TEXTURES];
    Texture planned[MAX_TEXTURES];
    Upload uploads[MAX_TEXTURES];
    Bgra *pixels;
    size_t pixel_capacity;
    Vertex *vertices;
    size_t vertex_capacity;
    uint32_t width, height;
    const char *error;
};

typedef char require_four_byte_pixels[sizeof(Bgra) == 4 ? 1 : -1];
typedef char require_four_byte_floats[sizeof(float) == 4 ? 1 : -1];

static uint32_t le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static float le_float(const uint8_t *data)
{
    const uint32_t bits = le32(data);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static const uint8_t *take(Reader *reader, size_t count)
{
    const uint8_t *result = reader->next;
    if (count > reader->remaining)
        return NULL;
    reader->next += count;
    reader->remaining -= count;
    return result;
}

static int dimensions(uint32_t width, uint32_t height, size_t *count)
{
    if (!width || !height || width > MAX_SIDE || height > MAX_SIDE ||
        width > MAX_BYTES / 4U / height)
        return 0;
    *count = (size_t)width * height;
    return 1;
}

static int texture_slot(const Texture *textures, uint32_t id, int allow_new)
{
    int vacant = -1;
    unsigned i;
    for (i = 0; i < MAX_TEXTURES; ++i) {
        if (textures[i].pixels) {
            if (textures[i].id == id)
                return (int)i;
        } else if (vacant < 0) {
            vacant = (int)i;
        }
    }
    return allow_new ? vacant : -1;
}

static void copy_texels(uint32_t *out, const uint8_t *in, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
        out[i] = le32(in + i * 4U);
}

static void discard_uploads(WcRaster *raster, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        free(raster->uploads[i].replacement);
        raster->uploads[i].replacement = NULL;
    }
}

/* Validate all texture updates against shadow metadata. Keep full replacements
 * private until the complete frame has passed validation. */
static int plan_uploads(WcRaster *raster, Reader *reader, uint32_t count)
{
    uint32_t i;
    size_t cache_size = 0;
    memcpy(raster->planned, raster->textures, sizeof(raster->planned));
    memset(raster->uploads, 0, count * sizeof(raster->uploads[0]));
    for (i = 0; i < MAX_TEXTURES; ++i) {
        const Texture *texture = &raster->textures[i];
        if (texture->pixels)
            cache_size += (size_t)texture->width * texture->height * 4U;
    }
    for (i = 0; i < count; ++i) {
        const uint8_t *header = take(reader, 20);
        Upload *upload = &raster->uploads[i];
        size_t pixels, bytes;
        int full, slot;
        Texture *texture;
        if (!header)
            return 0;
        upload->id = le32(header);
        upload->x = le32(header + 4);
        upload->y = le32(header + 8);
        upload->width = le32(header + 12);
        upload->height = le32(header + 16);
        if (!dimensions(upload->width, upload->height, &pixels))
            return 0;
        bytes = pixels * 4U;
        upload->pixels = take(reader, bytes);
        if (!upload->pixels)
            return 0;
        full = upload->x == UINT32_MAX && upload->y == UINT32_MAX;
        slot = texture_slot(raster->planned, upload->id, full);
        if (slot < 0)
            return 0;
        upload->slot = (unsigned)slot;
        texture = &raster->planned[slot];
        if (full) {
            if (texture->pixels)
                cache_size -= (size_t)texture->width * texture->height * 4U;
            if (bytes > MAX_BYTES - cache_size)
                return 0;
            upload->replacement = malloc(bytes);
            if (!upload->replacement) {
                raster->error = "Not enough memory for an egui texture.";
                return 0;
            }
            copy_texels(upload->replacement, upload->pixels, pixels);
            texture->id = upload->id;
            texture->width = upload->width;
            texture->height = upload->height;
            texture->pixels = upload->replacement;
            cache_size += bytes;
        } else if (upload->width > texture->width ||
                   upload->height > texture->height ||
                   upload->x > texture->width - upload->width ||
                   upload->y > texture->height - upload->height) {
            return 0;
        }
    }
    return 1;
}

static void apply_uploads(WcRaster *raster, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        Upload *upload = &raster->uploads[i];
        Texture *texture = &raster->textures[upload->slot];
        if (upload->replacement) {
            free(texture->pixels);
            texture->pixels = upload->replacement;
            upload->replacement = NULL;
            texture->id = upload->id;
            texture->width = upload->width;
            texture->height = upload->height;
        } else {
            uint32_t row;
            for (row = 0; row < upload->height; ++row) {
                const size_t dest = (size_t)(upload->y + row) *
                                    texture->width + upload->x;
                const size_t source = (size_t)row * upload->width * 4U;
                copy_texels(texture->pixels + dest, upload->pixels + source,
                            upload->width);
            }
        }
    }
}

static int read_mesh(Reader *reader, Mesh *mesh)
{
    const uint8_t *header = take(reader, 28);
    unsigned i;
    if (!header)
        return 0;
    mesh->id = le32(header);
    for (i = 0; i < 4; ++i) {
        mesh->clip[i] = le_float(header + 4 + 4U * i);
        if (mesh->clip[i] != mesh->clip[i])  /* NaN; infinity is a valid clip. */
            return 0;
    }
    mesh->vertex_count = le32(header + 20);
    mesh->index_count = le32(header + 24);
    if (mesh->vertex_count > MAX_VERTICES || mesh->index_count > MAX_INDICES ||
        mesh->index_count % 3U != 0)
        return 0;
    mesh->vertices = take(reader, (size_t)mesh->vertex_count * 20U);
    mesh->indices = take(reader, (size_t)mesh->index_count * 4U);
    return mesh->vertices && mesh->indices;
}

static int bounded_float(float value, float limit)
{
    return value >= -limit && value <= limit;
}

static int validate_mesh(const Mesh *mesh, const Texture *textures)
{
    uint32_t i;
    if (mesh->index_count && texture_slot(textures, mesh->id, 0) < 0)
        return 0;
    for (i = 0; i < mesh->vertex_count; ++i) {
        const uint8_t *vertex = mesh->vertices + 20U * i;
        /* Bound fixed-point edge products even for triangles far off screen. */
        if (!bounded_float(le_float(vertex), 1000000.0f) ||
            !bounded_float(le_float(vertex + 4), 1000000.0f) ||
            !bounded_float(le_float(vertex + 8), 16384.0f) ||
            !bounded_float(le_float(vertex + 12), 16384.0f))
            return 0;
    }
    for (i = 0; i < mesh->index_count; ++i)
        if (le32(mesh->indices + 4U * i) >= mesh->vertex_count)
            return 0;
    return 1;
}

static int32_t subpixel(float value)
{
    const double scaled = (double)value * SUBPIXEL;
    return (int32_t)(scaled + (scaled < 0.0 ? -0.5 : 0.5));
}

static int32_t clip_coordinate(float value, uint32_t max)
{
    if (value <= 0.0f)
        return 0;
    if (value >= (float)max)
        return (int32_t)max;
    return (int32_t)(value + 0.5f);
}

static int32_t min3(int32_t a, int32_t b, int32_t c)
{
    int32_t low = a < b ? a : b;
    return low < c ? low : c;
}

static int32_t max3(int32_t a, int32_t b, int32_t c)
{
    int32_t high = a > b ? a : b;
    return high > c ? high : c;
}

/* Convert subpixel bounds to an interval of pixel centers. */
static int32_t first_pixel(int32_t coordinate)
{
    const int32_t adjusted = coordinate - HALF_PIXEL;
    return adjusted >= 0 ? (adjusted + SUBPIXEL - 1) / SUBPIXEL :
                           adjusted / SUBPIXEL;
}

static int32_t past_pixel(int32_t coordinate)
{
    const int32_t adjusted = coordinate - HALF_PIXEL;
    return 1 + (adjusted >= 0 ? adjusted / SUBPIXEL :
                -((-adjusted + SUBPIXEL - 1) / SUBPIXEL));
}

static int64_t edge_value(const Vertex *a, const Vertex *b, int32_t x, int32_t y)
{
    return (int64_t)(b->x - a->x) * (y - a->y) -
           (int64_t)(b->y - a->y) * (x - a->x);
}

static Edge edge_setup(const Vertex *a, const Vertex *b, int32_t x, int32_t y)
{
    const int32_t dx = b->x - a->x;
    const int32_t dy = b->y - a->y;
    const int top_left = dy < 0 || (dy == 0 && dx > 0);
    Edge edge;
    edge.value = edge_value(a, b, x, y) - !top_left;
    edge.dx = -(int64_t)dy * SUBPIXEL;
    edge.dy = (int64_t)dx * SUBPIXEL;
    return edge;
}

/* Intersect a pixel-center scanline span with one top-left-rule edge. */
static int edge_span(const Edge *edge, int32_t *first, int32_t *past)
{
    int64_t bound;
    if (edge->dx > 0 && edge->value < 0) {
        bound = (-edge->value + edge->dx - 1) / edge->dx;
        if (bound >= *past)
            return 0;
        if (bound > *first)
            *first = (int32_t)bound;
    } else if (edge->dx < 0) {
        if (edge->value < 0)
            return 0;
        if (edge->value + edge->dx * (*past - 1) < 0) {
            bound = edge->value / -edge->dx + 1;
            if (bound <= *first)
                return 0;
            *past = (int32_t)bound;
        }
    } else if (edge->value < 0) {
        return 0;
    }
    return 1;
}

static Plane plane_setup(const Vertex *a, const Vertex *b, const Vertex *c,
                         double va, double vb, double vc, double inv_area)
{
    Plane plane;
    plane.dx = ((vb - va) * (c->y - a->y) - (vc - va) * (b->y - a->y)) *
               SUBPIXEL * inv_area;
    plane.dy = ((vc - va) * (b->x - a->x) - (vb - va) * (c->x - a->x)) *
               SUBPIXEL * inv_area;
    plane.start = va + plane.dx * (0.5 - (double)a->x / SUBPIXEL) +
                       plane.dy * (0.5 - (double)a->y / SUBPIXEL);
    return plane;
}

static double plane_at(const Plane *plane, int32_t x, int32_t y)
{
    return plane->start + plane->dx * x + plane->dy * y;
}

static unsigned channel(uint32_t color, unsigned index)
{
    return (unsigned)((color >> (8U * index)) & 255U);
}

static unsigned rounded_byte(double value)
{
    if (value <= 0.0)
        return 0;
    if (value >= 255.0)
        return 255;
    return (unsigned)(value + 0.5);
}

static unsigned mul255(unsigned left, unsigned right)
{
    const unsigned product = left * right + 128U;
    return (product + (product >> 8)) >> 8;
}

static uint32_t modulate(uint32_t texture, uint32_t vertex)
{
    unsigned i;
    uint32_t result = 0;
    if (texture == UINT32_MAX)
        return vertex;
    if (vertex == UINT32_MAX)
        return texture;
    for (i = 0; i < 4; ++i)
        result |= (uint32_t)mul255(channel(texture, i), channel(vertex, i)) <<
                  (8U * i);
    return result;
}

static uint32_t lerp(uint32_t a, uint32_t b, unsigned weight)
{
    const uint32_t mask = 0x00ff00ffUL;
    const unsigned inverse = 256U - weight;
    const uint32_t rb = (a & mask) * inverse + (b & mask) * weight + 0x00800080UL;
    const uint32_t ga = ((a >> 8) & mask) * inverse +
                        ((b >> 8) & mask) * weight + 0x00800080UL;
    return ((rb >> 8) & mask) | (ga & 0xff00ff00UL);
}

static unsigned sample_coord(double texel, uint32_t extent)
{
    if (texel <= 0.0)
        return 0;
    if (texel >= (double)(extent - 1U))
        return (extent - 1U) * 256U;
    return (unsigned)(texel * 256.0);
}

static uint32_t sample(const Texture *texture, double u, double v)
{
    const unsigned fx = sample_coord(u, texture->width);
    const unsigned fy = sample_coord(v, texture->height);
    const unsigned x = fx >> 8, y = fy >> 8;
    const uint32_t *row = texture->pixels + (size_t)y * texture->width;
    uint32_t top = row[x];
    if (fx & 255U)
        top = lerp(top, row[x + 1U], fx & 255U);
    if (fy & 255U) {
        uint32_t bottom;
        row += texture->width;
        bottom = row[x];
        if (fx & 255U)
            bottom = lerp(bottom, row[x + 1U], fx & 255U);
        top = lerp(top, bottom, fy & 255U);
    }
    return top;
}

static uint8_t saturate_byte(unsigned value)
{
    return (uint8_t)(value > 255U ? 255U : value);
}

static void blend(Bgra *pixel, uint32_t rgba)
{
    const unsigned alpha = channel(rgba, 3);
    if (alpha == 255U) {
        pixel->r = (uint8_t)rgba;
        pixel->g = (uint8_t)(rgba >> 8);
        pixel->b = (uint8_t)(rgba >> 16);
    } else if (rgba) {
        const unsigned inverse = 255U - alpha;
        pixel->r = saturate_byte(channel(rgba, 0) + mul255(pixel->r, inverse));
        pixel->g = saturate_byte(channel(rgba, 1) + mul255(pixel->g, inverse));
        pixel->b = saturate_byte(channel(rgba, 2) + mul255(pixel->b, inverse));
    }
}

static void solid_span(Bgra *pixel, int32_t count, uint32_t rgba)
{
    int32_t i;
    if (rgba >= 0xff000000UL) {
        Bgra value;
        value.b = (uint8_t)(rgba >> 16);
        value.g = (uint8_t)(rgba >> 8);
        value.r = (uint8_t)rgba;
        value.a = 255;
        for (i = 0; i < count; ++i)
            pixel[i] = value;
    } else if (rgba) {
        for (i = 0; i < count; ++i)
            blend(pixel + i, rgba);
    }
}

static void triangle(Bgra *surface, uint32_t stride, Clip clip,
                     const Texture *texture, const Vertex *a, const Vertex *b,
                     const Vertex *c)
{
    int64_t area = edge_value(a, b, c->x, c->y);
    int32_t left, right, top, bottom, y;
    Edge edges[3];
    Plane u_plane, v_plane, color_plane[4];
    uint32_t flat_texel = UINT32_MAX;
    int flat_color, flat_uv;
    unsigned i;

    if (!area)
        return;
    if (area < 0) {
        const Vertex *swap = b;
        b = c;
        c = swap;
        area = -area;
    }

    left = first_pixel(min3(a->x, b->x, c->x));
    right = past_pixel(max3(a->x, b->x, c->x));
    top = first_pixel(min3(a->y, b->y, c->y));
    bottom = past_pixel(max3(a->y, b->y, c->y));
    if (left < clip.left) left = clip.left;
    if (right > clip.right) right = clip.right;
    if (top < clip.top) top = clip.top;
    if (bottom > clip.bottom) bottom = clip.bottom;
    if (left >= right || top >= bottom)
        return;

    edges[0] = edge_setup(a, b, left * SUBPIXEL + HALF_PIXEL,
                         top * SUBPIXEL + HALF_PIXEL);
    edges[1] = edge_setup(b, c, left * SUBPIXEL + HALF_PIXEL,
                         top * SUBPIXEL + HALF_PIXEL);
    edges[2] = edge_setup(c, a, left * SUBPIXEL + HALF_PIXEL,
                         top * SUBPIXEL + HALF_PIXEL);

    flat_color = a->color == b->color && a->color == c->color;
    flat_uv = a->u == b->u && a->u == c->u && a->v == b->v && a->v == c->v;
    if (flat_uv) {
        flat_texel = sample(texture, (double)a->u * texture->width - 0.5,
                            (double)a->v * texture->height - 0.5);
    } else {
        const double inv_area = 1.0 / (double)area;
        u_plane = plane_setup(a, b, c, (double)a->u * texture->width - 0.5,
                             (double)b->u * texture->width - 0.5,
                             (double)c->u * texture->width - 0.5, inv_area);
        v_plane = plane_setup(a, b, c, (double)a->v * texture->height - 0.5,
                             (double)b->v * texture->height - 0.5,
                             (double)c->v * texture->height - 0.5, inv_area);
    }
    if (!flat_color) {
        const double inv_area = 1.0 / (double)area;
        for (i = 0; i < 4; ++i)
            color_plane[i] = plane_setup(a, b, c, channel(a->color, i),
                                         channel(b->color, i), channel(c->color, i),
                                         inv_area);
    }

    for (y = top; y < bottom; ++y) {
        int32_t start = 0, past = right - left;
        if (edge_span(&edges[0], &start, &past) &&
            edge_span(&edges[1], &start, &past) &&
            edge_span(&edges[2], &start, &past)) {
            Bgra *pixel = surface + (size_t)y * stride + (left + start);
            if (flat_color && flat_uv) {
                solid_span(pixel, past - start, modulate(flat_texel, a->color));
            } else {
                double u = 0.0, v = 0.0, colors[4] = {0.0, 0.0, 0.0, 0.0};
                int32_t x;
                if (!flat_uv) {
                    u = plane_at(&u_plane, left + start, y);
                    v = plane_at(&v_plane, left + start, y);
                }
                if (!flat_color)
                    for (i = 0; i < 4; ++i)
                        colors[i] = plane_at(&color_plane[i], left + start, y);
                for (x = start; x < past; ++x, ++pixel) {
                    uint32_t texel = flat_texel;
                    uint32_t color = a->color;
                    if (!flat_uv) {
                        texel = sample(texture, u, v);
                        u += u_plane.dx;
                        v += v_plane.dx;
                    }
                    if (!flat_color) {
                        color = 0;
                        for (i = 0; i < 4; ++i) {
                            color |= (uint32_t)rounded_byte(colors[i]) << (8U * i);
                            colors[i] += color_plane[i].dx;
                        }
                    }
                    blend(pixel, modulate(texel, color));
                }
            }
        }
        for (i = 0; i < 3; ++i)
            edges[i].value += edges[i].dy;
    }
}

static void paint_mesh(WcRaster *raster, const Mesh *mesh,
                       uint32_t width, uint32_t height)
{
    Clip clip;
    uint32_t i;
    int slot;
    if (!mesh->index_count)
        return;
    clip.left = clip_coordinate(mesh->clip[0], width);
    clip.top = clip_coordinate(mesh->clip[1], height);
    clip.right = clip_coordinate(mesh->clip[2], width);
    clip.bottom = clip_coordinate(mesh->clip[3], height);
    if (clip.left >= clip.right || clip.top >= clip.bottom)
        return;
    slot = texture_slot(raster->textures, mesh->id, 0);
    for (i = 0; i < mesh->vertex_count; ++i) {
        const uint8_t *input = mesh->vertices + i * 20U;
        Vertex *vertex = &raster->vertices[i];
        vertex->x = subpixel(le_float(input));
        vertex->y = subpixel(le_float(input + 4));
        vertex->u = le_float(input + 8);
        vertex->v = le_float(input + 12);
        vertex->color = le32(input + 16);
    }
    for (i = 0; i < mesh->index_count; i += 3) {
        const uint8_t *index = mesh->indices + i * 4U;
        triangle(raster->pixels, width, clip, &raster->textures[slot],
                 raster->vertices + le32(index),
                 raster->vertices + le32(index + 4),
                 raster->vertices + le32(index + 8));
    }
}

static int reserve_frame(WcRaster *raster, size_t pixels, size_t vertices)
{
    if (vertices > raster->vertex_capacity) {
        Vertex *replacement = realloc(raster->vertices, vertices * sizeof(Vertex));
        if (!replacement)
            return 0;
        raster->vertices = replacement;
        raster->vertex_capacity = vertices;
    }
    if (pixels > raster->pixel_capacity) {
        Bgra *replacement = realloc(raster->pixels, pixels * sizeof(Bgra));
        if (!replacement)
            return 0;
        raster->pixels = replacement;
        raster->pixel_capacity = pixels;
    }
    return 1;
}

WcRaster *wc_raster_create(void)
{
    return calloc(1, sizeof(WcRaster));
}

void wc_raster_destroy(WcRaster *raster)
{
    unsigned i;
    if (!raster)
        return;
    for (i = 0; i < MAX_TEXTURES; ++i)
        free(raster->textures[i].pixels);
    free(raster->pixels);
    free(raster->vertices);
    free(raster);
}

int wc_raster_frame(WcRaster *raster, uint32_t width, uint32_t height,
                    const void *data, size_t len)
{
    static const Bgra backdrop = {240, 240, 240, 255};
    Reader reader, meshes;
    const uint8_t *header, *frees;
    uint32_t set_count, mesh_count, free_count, i;
    size_t pixel_count, max_vertices = 0, pixel;
    Mesh mesh;

    if (!raster)
        return 0;
    raster->error = "Invalid WCG1 frame or display size.";
    if (!data || len < 16 || len > MAX_BYTES ||
        !dimensions(width, height, &pixel_count))
        return 0;
    reader.next = data;
    reader.remaining = len;
    header = take(&reader, 16);
    if (memcmp(header, "WCG1", 4) != 0)
        return 0;
    set_count = le32(header + 4);
    mesh_count = le32(header + 8);
    free_count = le32(header + 12);
    if (set_count > MAX_TEXTURES || mesh_count > MAX_MESHES || free_count > MAX_TEXTURES)
        return 0;
    raster->error = "Invalid WCG1 texture update.";
    if (!plan_uploads(raster, &reader, set_count))
        goto invalid;
    meshes = reader;
    raster->error = "Invalid WCG1 mesh or texture reference.";
    for (i = 0; i < mesh_count; ++i) {
        if (!read_mesh(&reader, &mesh) || !validate_mesh(&mesh, raster->planned))
            goto invalid;
        if (mesh.vertex_count > max_vertices)
            max_vertices = mesh.vertex_count;
    }
    raster->error = "Invalid WCG1 texture removal list.";
    frees = take(&reader, (size_t)free_count * 4U);
    if (!frees || reader.remaining)
        goto invalid;
    raster->error = "Not enough memory for the WordCraft frame.";
    if (!reserve_frame(raster, pixel_count, max_vertices))
        goto invalid;

    /* The packet has been validated and all allocations have succeeded.
     * PM reads the completed pixels on this same thread. */
    apply_uploads(raster, set_count);
    for (pixel = 0; pixel < pixel_count; ++pixel)
        raster->pixels[pixel] = backdrop;
    for (i = 0; i < mesh_count; ++i) {
        /* The same immutable packet was completely checked above. */
        (void)read_mesh(&meshes, &mesh);
        paint_mesh(raster, &mesh, width, height);
    }
    for (i = 0; i < free_count; ++i) {
        const int slot = texture_slot(raster->textures, le32(frees + 4U * i), 0);
        if (slot >= 0) {
            free(raster->textures[slot].pixels);
            raster->textures[slot].pixels = NULL;
        }
    }

    raster->width = width;
    raster->height = height;
    raster->error = NULL;
    return 1;

invalid:
    discard_uploads(raster, set_count);
    return 0;
}

const uint8_t *wc_raster_pixels(const WcRaster *raster, uint32_t *width,
                                uint32_t *height, size_t *stride)
{
    if (!raster)
        return NULL;
    if (width)
        *width = raster->width;
    if (height)
        *height = raster->height;
    if (stride)
        *stride = (size_t)raster->width * 4U;
    return (const uint8_t *)raster->pixels;
}

const char *wc_raster_error(const WcRaster *raster)
{
    return raster && raster->error ? raster->error : "";
}
