/* nn::pl shared fonts.
 *
 * The console keeps the six system fonts as BFTTF: an 8-byte header followed by an obfuscated sfnt, one fixed key XORed
 * into every 32-bit word. A game never decodes that itself. It hands the address to
 * nn::fontll::ScalableFontEngineHelper::Decode (sdk+0x18e424 in this title's SDK) and uses what comes back: the same
 * address + 8. Decode returns early when the first word already reads as decoded (0x18029a7f, sdk+0x18e438), and
 * otherwise XOR-decodes the payload in place with key 0x49621806 and the length from the header's second word
 * (bswap(length) ^ key, sdk+0x18e488). So GetSharedFontAddress has to hand out a pointer to such a header and the size
 * has to count that header -- serving a bare sfnt there makes Decode read the typeface's own first word as a header.
 *
 * The real fonts live in the console firmware. A build without them installs stand-ins instead: a host typeface behind
 * a header that already carries the decoded marker, so Decode passes it through untouched. That is the substitution the
 * reference port for this technique makes, and it is sound here because the game's own text comes from its RomFS -- the
 * shared fonts only cover system names and the button glyphs, and a missing button glyph draws as "?".
 *
 * scripts/extract_shared_fonts.py writes the firmware fonts to <work>/extracted/firmware/shared-fonts; rows[] below is
 * generated from its manifest and, when present, those files are served byte for byte, exactly as hardware serves them.
 */
#include "runtime.h"
#include "shared_fonts_config.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define BFTTF_MARKER 0x18029a7fu
#define BFTTF_KEY 0x49621806u
typedef struct{const char *path;uint64_t file_bytes,font_bytes;const char *sha256;} FontRow;
static const FontRow rows[SHARED_FONT_COUNT]={SHARED_FONT_ROWS};
static const char *const names[SHARED_FONT_COUNT]={"standard","chinese-simplified","chinese-simplified-extension",
                                                   "chinese-traditional","korean","nintendo-extension"};
static uint8_t *region;                       /* every font laid out back to back, headers included */
static uint64_t offsets[SHARED_FONT_COUNT];   /* where each font starts in region */
static uint64_t lengths[SHARED_FONT_COUNT];   /* header + payload: the size the game is told */
static int plain;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void need(int c,const char *m){if(!c)runtime_fail(m);}
/* Wrap a host typeface the way the console's shared memory would hold it, with the header already marked decoded. */
static uint64_t host_font(uint8_t **out,const char *path){
 int fd=open(path,O_RDONLY);
 if(fd<0)return 0;
 struct stat st;
 if(fstat(fd,&st)||st.st_size<=0){close(fd);return 0;}
 size_t n=(size_t)st.st_size;uint8_t *m=malloc(n+8);
 if(!m){close(fd);return 0;}
 size_t got=0;
 while(got<n){ssize_t r=pread(fd,m+8+got,n-got,(off_t)got);if(r<=0)break;got+=(size_t)r;}
 close(fd);
 if(got!=n){free(m);return 0;}
 uint32_t marker=BFTTF_MARKER,len=__builtin_bswap32((uint32_t)n^BFTTF_KEY);
 memcpy(m,&marker,4);memcpy(m+4,&len,4);
 fprintf(stderr,"[totk-pl] stand-in shared font: %s (%zu byte payload)\n",path,n);
 *out=m;return n+8;
}
static const char *env_or_dir(const char *env,const char *file,char *buf,size_t size){
 const char *direct=getenv(env),*dir=getenv("TOTK_SHARED_FONTS");
 if(direct&&*direct)return direct;
 if(dir&&*dir){snprintf(buf,size,"%s/%s",dir,file);return buf;}
 return NULL;
}
static void load(void){
 /* TOTK_SHARED_FONT_PLAIN=1: hand out the bare typeface, for a title whose font path skips Decode. */
 const char *p=getenv("TOTK_SHARED_FONT_PLAIN");plain=p&&*p&&strcmp(p,"0");
 if(rows[0].path[0]){
  uint64_t total=0;for(int i=0;i<SHARED_FONT_COUNT;i++)total+=rows[i].file_bytes;
  region=malloc(total);need(region!=NULL,"shared font memory");
  uint64_t at=0;
  for(int i=0;i<SHARED_FONT_COUNT;i++){
   /* TOTK_SHARED_FONTS=<dir>: the same verified files in another directory (iOS: copied into the app's Documents). */
   char path[4096];const char *dir=getenv("TOTK_SHARED_FONTS"),*base=strrchr(rows[i].path,'/');
   if(dir)need(snprintf(path,sizeof path,"%s/%s",dir,base?base+1:rows[i].path)<(int)sizeof path,"shared font path");
   FILE *f=fopen(dir?path:rows[i].path,"rb");need(f!=NULL,"open shared font");
   need(fread(region+at,1,rows[i].file_bytes,f)==rows[i].file_bytes&&fgetc(f)==EOF,"shared font size");fclose(f);
   uint8_t d[32];char hex[65];CC_SHA256(region+at,(CC_LONG)rows[i].file_bytes,d);for(int k=0;k<32;k++)snprintf(hex+k*2,3,"%02x",d[k]);
   need(!strcmp(hex,rows[i].sha256),"shared font integrity mismatch");
   offsets[i]=at;lengths[i]=rows[i].file_bytes;at+=rows[i].file_bytes;
  }
  fprintf(stderr,"[totk-pl] shared fonts: %d firmware fonts, %llu bytes\n",SHARED_FONT_COUNT,(unsigned long long)total);
  return;
 }
 /* No firmware fonts: one typeface for types 0-4 and, when it exists, a second for the button glyphs (type 5). */
 char buf[4096];
 const char *std_path=env_or_dir("TOTK_SHARED_FONT","standard.ttf",buf,sizeof buf);
 if(!std_path)std_path="/System/Library/Fonts/Supplemental/Arial Unicode.ttf";
 uint8_t *std_image=NULL;uint64_t std_bytes=host_font(&std_image,std_path);
 need(std_bytes!=0,"no stand-in shared font: set TOTK_SHARED_FONT to a TTF");
 char ext_buf[4096];
 const char *ext_path=env_or_dir("TOTK_SHARED_FONT_EXT","nintendo_ext.ttf",ext_buf,sizeof ext_buf);
 uint8_t *ext_image=NULL;uint64_t ext_bytes=ext_path?host_font(&ext_image,ext_path):0;
 if(ext_path&&!ext_bytes)fprintf(stderr,"[totk-pl] no extension font at %s: button glyphs will draw as \"?\"\n",ext_path);
 region=malloc(std_bytes+ext_bytes);need(region!=NULL,"shared font memory");
 memcpy(region,std_image,std_bytes);free(std_image);
 for(int i=0;i<SHARED_FONT_COUNT;i++){offsets[i]=0;lengths[i]=std_bytes;}
 if(ext_bytes){
  memcpy(region+std_bytes,ext_image,ext_bytes);free(ext_image);
  offsets[5]=std_bytes;lengths[5]=ext_bytes;
 }
 fprintf(stderr,"[totk-pl] shared fonts: stand-ins for %d types, %llu bytes\n",SHARED_FONT_COUNT,
         (unsigned long long)(std_bytes+ext_bytes));
}
static int valid(int type){
 if(type<0||type>=SHARED_FONT_COUNT)runtime_fail("invalid shared font type");
 /* Which types the game asks for says where its text comes from: type 0 alone is system names and button glyphs, a
    CJK type means UI text that is not in the game's own assets and a stand-in cannot cover it. */
 static unsigned seen;
 if(!(seen&(1u<<type))){seen|=1u<<type;fprintf(stderr,"[totk-pl] shared font type %d (%s) requested\n",type,names[type]);}
 pthread_once(&once,load);
 return type;
}
static int load_state(int type){valid(type);return 1;}   /* SharedFontLoadState_Loaded */
static const void *address(int type){return region+offsets[valid(type)]+(plain?8:0);}
static size_t size(int type){return (size_t)(lengths[valid(type)]-(plain?8:0));}
static void request_load(int type){(void)type;}   /* the fonts are there on demand, nothing to wait for */
/* Every font at once. The pl service lists them in type order and its addresses point past each font's header, which is
   why a title using this call hands the pointer straight to LoadFont instead of decoding it (sdk+0x388350). */
typedef struct{uint32_t type,pad;const void *address;uint64_t size;} FontData;
static unsigned get_all(int *count,FontData *out,int max,uint64_t language){
 (void)language;
 pthread_once(&once,load);
 int n=0;
 for(int t=0;t<SHARED_FONT_COUNT;t++){
  if(max>0&&out&&n<max){out[n].type=(uint32_t)t;out[n].pad=0;out[n].address=region+offsets[t]+8;out[n].size=lengths[t]-8;}
  n++;
 }
 if(count)*count=n;
 return (unsigned)(n>0);
}
static int not_vr(void){return 0;}
static void enter_vr(void){fprintf(stderr,"[totk-pl] pl::BeginVrMode (the host never enters VR)\n");}
static void leave_vr(void){}
void *runtime_font_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
  {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",load_state},
  {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",address},
  {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",size},
  {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE",request_load},
  {"_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE",get_all},
  {"_ZN2nn2pl8IsVrModeEv",not_vr},
  {"_ZN2nn2pl11BeginVrModeEv",enter_vr},
  {"_ZN2nn2pl9EndVrModeEv",leave_vr}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
