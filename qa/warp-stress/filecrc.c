/* Fast transport-integrity check. CRC32 is not a security checksum. */
#include <stdio.h>
#include <stdint.h>

static uint32_t table[256];
static unsigned char block[65536];
int main(int argc,char **argv) {
    unsigned i,j;int k,failed=0;
    if(argc<2||argc>33)return 2;
    for(i=0;i<256;i++) {
        uint32_t c=i;
        for(j=0;j<8;j++)c=(c>>1)^((c&1)?UINT32_C(0xedb88320):0);
        table[i]=c;
    }
    for(k=1;k<argc;k++) {
        FILE *f=fopen(argv[k],"rb");uint32_t crc=UINT32_C(0xffffffff);unsigned long size=0;size_t n;
        if(!f) { printf("ERROR %s\n",argv[k]);failed=1;continue; }
        while((n=fread(block,1,sizeof(block),f))!=0) {
            size+=(unsigned long)n;
            for(i=0;i<n;i++)crc=(crc>>8)^table[(crc^block[i])&255];
        }
        if(ferror(f)) { printf("ERROR %s\n",argv[k]);failed=1; }
        else printf("%08lx %lu %s\n",(unsigned long)(crc^UINT32_C(0xffffffff)),size,argv[k]);
        fclose(f);
    }
    return failed;
}
