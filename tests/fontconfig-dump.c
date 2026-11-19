#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const queries[] = {
    "sans-serif", "serif", "monospace", "cursive", "fantasy",
    "DejaVu Sans", "DejaVu Serif", "DejaVu Sans Mono",
    "Arial", "Helvetica", "Times New Roman", "Courier New",
    "Liberation Sans", "Noto Sans", "Noto Serif", "Noto Sans Mono",
    "sans-serif:weight=bold", "serif:slant=italic",
    "monospace:weight=light", "sans-serif:lang=ja",
    "serif:lang=zh-cn", "sans-serif:lang=ar", "sans-serif:lang=he",
    "sans-serif:pixelsize=13", "serif:pixelsize=16:weight=bold",
    "sans-serif:width=condensed", "sans-serif:spacing=proportional",
    "sans-serif:spacing=mono", "sans-serif:spacing=charcell",
    "DejaVu Sans:style=Oblique", "sans-serif:fontversion=0",
    "sans-serif:file=/nonexistent", "sans-serif:index=1",
};

static int cmpstr(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(int argc, char **argv) {
  FcConfig *c = FcInitLoadConfigAndFonts();
  FcStrList *list;
  FcChar8 *s;
  if (!c) {
    puts("CONFIG-NULL");
    return 1;
  }
  list = FcConfigGetConfigFiles(c);
  if (list) {
    while ((s = FcStrListNext(list)) != NULL)
      printf("file %s\n", (const char *)s);
    FcStrListDone(list);
  }
  list = FcConfigGetConfigDirs(c);
  if (list) {
    while ((s = FcStrListNext(list)) != NULL)
      printf("dir %s\n", (const char *)s);
    FcStrListDone(list);
  }
  for (int i = 1; i < argc; i++) {
    FcConfig *d = FcConfigCreate();
    FcBool ok = FcConfigParseAndLoad(d, (const FcChar8 *)argv[i], FcTrue);
    printf("parse %s %d\n", argv[i], (int)ok);
    FcConfigDestroy(d);
  }
  if (!FcConfigBuildFonts(c)) {
    puts("BUILD-FONTS-FAILED");
    return 1;
  }
  {
    FcFontSet *fs = FcConfigGetFonts(c, FcSetSystem);
    char **names = NULL;
    int n = fs ? fs->nfont : 0;
    int k = 0;
    if (n) {
      names = calloc((size_t)n, sizeof(*names));
      for (int i = 0; i < n; i++) {
        char *u = (char *)FcNameUnparse(fs->fonts[i]);
        if (u) {
          names[k++] = u;
        }
      }
      qsort(names, (size_t)k, sizeof(*names), cmpstr);
      for (int i = 0; i < k; i++) {
        printf("font %s\n", names[i]);
        free(names[i]);
      }
      free(names);
    }
  }
  for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
    FcPattern *pat = FcNameParse((const FcChar8 *)queries[i]);
    FcResult res = FcResultNoMatch;
    FcPattern *m;
    char *u;
    if (!pat) {
      printf("match %s NULL\n", queries[i]);
      continue;
    }
    FcConfigSubstitute(c, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    m = FcFontMatch(c, pat, &res);
    u = m ? (char *)FcNameUnparse(m) : NULL;
    printf("match %s %d %s\n", queries[i], (int)res, u ? u : "(none)");
    free(u);
    if (m)
      FcPatternDestroy(m);
    FcPatternDestroy(pat);
  }
  FcConfigDestroy(c);
  FcFini();
  return 0;
}
