/* Direct reads into caller-owned storage. MIT OR Apache-2.0. */
#include <stdio.h>
#include <stdlib.h>
#include "input.h"

struct WcInput {
    FILE *stream;
    uint32_t size;
};

const char *wc_input_open(const char *path, uint32_t size, WcInput **output)
{
    FILE *stream;
    WcInput *input;
    if (!output) return "Could not allocate input storage.";
    *output = NULL;
    if (!path || !path[0]) return "Could not open the selected file.";
    stream = fopen(path, "rb");
    if (!stream) return "Could not open the selected file.";
    input = malloc(sizeof(*input));
    if (!input) {
        fclose(stream);
        return "Not enough memory to open the selected file.";
    }
    input->stream = stream;
    input->size = size;
    *output = input;
    return NULL;
}

const char *wc_input_read_close(WcInput *input, void *destination, uint32_t capacity)
{
    size_t got = 0;
    int trailing, ok;
    if (!input) return "Could not read the whole file.";
    if (capacity != input->size || (capacity && !destination)) {
        wc_input_discard(input);
        return "The file input storage has the wrong size.";
    }
    if (capacity) got = fread(destination, 1, capacity, input->stream);
    trailing = fgetc(input->stream);
    ok = got == capacity && trailing == EOF && !ferror(input->stream);
    if (fclose(input->stream) != 0) ok = 0;
    free(input);
    return ok ? NULL : "Could not read the whole file; it may have changed while opening.";
}

void wc_input_discard(WcInput *input)
{
    if (!input) return;
    fclose(input->stream);
    free(input);
}
