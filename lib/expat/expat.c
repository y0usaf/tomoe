#include <expat.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define R_ERROR (-1)
#define R_OK 0
#define R_WAIT 1

enum { STATE_PROLOG = 0, STATE_CONTENT = 1, STATE_EPILOG = 2 };

typedef struct Ent {
  char *name;
  char *text;
  size_t textlen;
  int external;
  int unparsed;
  int parameter;
  int open;
} Ent;

typedef struct {
  size_t ns, nl;
  size_t noff, voff;
  size_t vs, vl;
  XML_Size vline;
} Attr;

typedef struct {
  char *name;
  int tokenized;
  char *dflt;
  size_t dlen;
  int has_dflt;
} AttDef;

typedef struct {
  char *name;
  AttDef *attrs;
  size_t n, cap;
} ElemDef;

typedef struct {
  unsigned char *buf;
  size_t len, cap, pos;
  int skip, state, in_cdata, in_comment, fixed, final;
  XML_Size line, fixedno, refline;
  size_t base;
  int ent_base;
  Ent *ent;
} EntFrame;

typedef struct {
  size_t pos;
  XML_Size line;
  int skip;
  size_t worklen, nav;
  unsigned long long direct, indirect;
} Snap;

struct XML_ParserStruct {
  void *ud;
  XML_StartElementHandler start;
  XML_EndElementHandler end;
  XML_CharacterDataHandler chars;
  XML_ProcessingInstructionHandler pi;
  XML_CommentHandler comment;
  XML_StartDoctypeDeclHandler dtstart;
  XML_EndDoctypeDeclHandler dtend;

  unsigned char *buf;
  size_t len, cap, pos;
  int final, done, got, wait;
  enum XML_Error err;
  XML_Size line;
  int skip;

  unsigned char *raw;
  size_t rawlen, rawcap, rawpos;
  int enc;
  enum XML_Error dec_err;

  int state;
  int at_start;
  int has_ext;
  int in_cdata;
  XML_Size cd_tline;
  int in_comment;
  XML_Size cm_tline;

  int in_entity;
  int ent_base;
  int fixed_line;
  XML_Size fixed_line_no;
  EntFrame *eframes;
  size_t efn, efcap;
  unsigned long long direct, indirect;

  unsigned char *dtd;
  size_t dtdlen, dtdcap, dtdpos;
  int dt_active, dt_subset, dt_have_header, dtskip;
  XML_Size dtline;
  char *dt_name, *dt_sys, *dt_pub;
  int seen_dt;

  struct Ent *ents;
  size_t nent, entcap;
  int has_param;
  ElemDef *edefs;
  size_t nedef, edefcap;

  char **stack;
  size_t depth, stackcap;

  char *work;
  size_t worklen, workcap;

  Attr *av;
  size_t nav, avcap;

  const char **attp;
  size_t attpcap;
};

static const char *const errmsg[] = {
    NULL,
    "out of memory",
    "syntax error",
    "no element found",
    "not well-formed (invalid token)",
    "unclosed token",
    "partial character",
    "mismatched tag",
    "duplicate attribute",
    "junk after document element",
    "illegal parameter entity reference",
    "undefined entity",
    "recursive entity reference",
    "asynchronous entity",
    "reference to invalid character number",
    "reference to binary entity",
    "reference to external entity in attribute",
    "XML or text declaration not at start of entity",
    "unknown encoding",
    "encoding specified in XML declaration is incorrect",
    "unclosed CDATA section",
    "error in processing external entity reference",
    "document is not standalone",
    "unexpected parser state - please send a bug report",
    "entity declared in parameter entity",
    "requested feature requires XML_DTD support in Expat",
    "cannot change setting once parsing has begun",
    "unbound prefix",
    "must not undeclare prefix",
    "incomplete markup in parameter entity",
    "XML declaration not well-formed",
    "text declaration not well-formed",
    "illegal character(s) in public id",
    "parser suspended",
    "parser not suspended",
    "parsing aborted",
    "parsing finished",
    "cannot suspend in external parameter entity",
    "reserved prefix (xml) must not be undeclared or bound to another "
    "namespace name",
    "reserved prefix (xmlns) must not be declared or undeclared",
    "prefix must not be bound to one of the reserved namespace names",
    "invalid argument",
    "a successful prior call to function XML_GetBuffer is required",
    "limit on input amplification factor (from DTD and entities) breached",
    "parser not started",
};

static int fail(struct XML_ParserStruct *p, enum XML_Error e, XML_Size line) {
  p->err = e;
  p->line = line;
  return R_ERROR;
}

static int nomem(struct XML_ParserStruct *p) {
  return fail(p, XML_ERROR_NO_MEMORY, p->line);
}

static const unsigned short nm_start[][2] = {
  {0xC0,0xD6},{0xD8,0xF6},{0xF8,0x131},{0x134,0x13E},{0x141,0x148},
  {0x14A,0x17E},{0x180,0x1C3},{0x1CD,0x1F0},{0x1F4,0x1F5},{0x1FA,0x217},
  {0x250,0x2A8},{0x2BB,0x2C1},{0x386,0x386},{0x388,0x38A},{0x38C,0x38C},
  {0x38E,0x3A1},{0x3A3,0x3CE},{0x3D0,0x3D6},{0x3DA,0x3DA},{0x3DC,0x3DC},
  {0x3DE,0x3DE},{0x3E0,0x3E0},{0x3E2,0x3F3},{0x401,0x40C},{0x40E,0x44F},
  {0x451,0x45C},{0x45E,0x481},{0x490,0x4C4},{0x4C7,0x4C8},{0x4CB,0x4CC},
  {0x4D0,0x4EB},{0x4EE,0x4F5},{0x4F8,0x4F9},{0x531,0x556},{0x559,0x559},
  {0x561,0x586},{0x5D0,0x5EA},{0x5F0,0x5F2},{0x621,0x63A},{0x641,0x64A},
  {0x671,0x6B7},{0x6BA,0x6BE},{0x6C0,0x6CE},{0x6D0,0x6D3},{0x6D5,0x6D5},
  {0x6E5,0x6E6},{0x905,0x939},{0x93D,0x93D},{0x958,0x961},{0x985,0x98C},
  {0x98F,0x990},{0x993,0x9A8},{0x9AA,0x9B0},{0x9B2,0x9B2},{0x9B6,0x9B9},
  {0x9DC,0x9DD},{0x9DF,0x9E1},{0x9F0,0x9F1},{0xA05,0xA0A},{0xA0F,0xA10},
  {0xA13,0xA28},{0xA2A,0xA30},{0xA32,0xA33},{0xA35,0xA36},{0xA38,0xA39},
  {0xA59,0xA5C},{0xA5E,0xA5E},{0xA72,0xA74},{0xA85,0xA8B},{0xA8D,0xA8D},
  {0xA8F,0xA91},{0xA93,0xAA8},{0xAAA,0xAB0},{0xAB2,0xAB3},{0xAB5,0xAB9},
  {0xABD,0xABD},{0xAE0,0xAE0},{0xB05,0xB0C},{0xB0F,0xB10},{0xB13,0xB28},
  {0xB2A,0xB30},{0xB32,0xB33},{0xB36,0xB39},{0xB3D,0xB3D},{0xB5C,0xB5D},
  {0xB5F,0xB61},{0xB85,0xB8A},{0xB8E,0xB90},{0xB92,0xB95},{0xB99,0xB9A},
  {0xB9C,0xB9C},{0xB9E,0xB9F},{0xBA3,0xBA4},{0xBA8,0xBAA},{0xBAE,0xBB5},
  {0xBB7,0xBB9},{0xC05,0xC0C},{0xC0E,0xC10},{0xC12,0xC28},{0xC2A,0xC33},
  {0xC35,0xC39},{0xC60,0xC61},{0xC85,0xC8C},{0xC8E,0xC90},{0xC92,0xCA8},
  {0xCAA,0xCB3},{0xCB5,0xCB9},{0xCDE,0xCDE},{0xCE0,0xCE1},{0xD05,0xD0C},
  {0xD0E,0xD10},{0xD12,0xD28},{0xD2A,0xD39},{0xD60,0xD61},{0xE01,0xE2E},
  {0xE30,0xE30},{0xE32,0xE33},{0xE40,0xE45},{0xE81,0xE82},{0xE84,0xE84},
  {0xE87,0xE88},{0xE8A,0xE8A},{0xE8D,0xE8D},{0xE94,0xE97},{0xE99,0xE9F},
  {0xEA1,0xEA3},{0xEA5,0xEA5},{0xEA7,0xEA7},{0xEAA,0xEAB},{0xEAD,0xEAE},
  {0xEB0,0xEB0},{0xEB2,0xEB3},{0xEBD,0xEBD},{0xEC0,0xEC4},{0xF40,0xF47},
  {0xF49,0xF69},{0x10A0,0x10C5},{0x10D0,0x10F6},{0x1100,0x1100},
  {0x1102,0x1103},{0x1105,0x1107},{0x1109,0x1109},{0x110B,0x110C},
  {0x110E,0x1112},{0x113C,0x113C},{0x113E,0x113E},{0x1140,0x1140},
  {0x114C,0x114C},{0x114E,0x114E},{0x1150,0x1150},{0x1154,0x1155},
  {0x1159,0x1159},{0x115F,0x1161},{0x1163,0x1163},{0x1165,0x1165},
  {0x1167,0x1167},{0x1169,0x1169},{0x116D,0x116E},{0x1172,0x1173},
  {0x1175,0x1175},{0x119E,0x119E},{0x11A8,0x11A8},{0x11AB,0x11AB},
  {0x11AE,0x11AF},{0x11B7,0x11B8},{0x11BA,0x11BA},{0x11BC,0x11C2},
  {0x11EB,0x11EB},{0x11F0,0x11F0},{0x11F9,0x11F9},{0x1E00,0x1E9B},
  {0x1EA0,0x1EF9},{0x1F00,0x1F15},{0x1F18,0x1F1D},{0x1F20,0x1F45},
  {0x1F48,0x1F4D},{0x1F50,0x1F57},{0x1F59,0x1F59},{0x1F5B,0x1F5B},
  {0x1F5D,0x1F5D},{0x1F5F,0x1F7D},{0x1F80,0x1FB4},{0x1FB6,0x1FBC},
  {0x1FBE,0x1FBE},{0x1FC2,0x1FC4},{0x1FC6,0x1FCC},{0x1FD0,0x1FD3},
  {0x1FD6,0x1FDB},{0x1FE0,0x1FEC},{0x1FF2,0x1FF4},{0x1FF6,0x1FFC},
  {0x2126,0x2126},{0x212A,0x212B},{0x212E,0x212E},{0x2180,0x2182},
  {0x3007,0x3007},{0x3021,0x3029},{0x3041,0x3094},{0x30A1,0x30FA},
  {0x3105,0x312C},{0x4E00,0x9FA5},{0xAC00,0xD7A3},
};
static const unsigned short nm_cont[][2] = {
  {0xB7,0xB7},{0xC0,0xD6},{0xD8,0xF6},{0xF8,0x131},{0x134,0x13E},
  {0x141,0x148},{0x14A,0x17E},{0x180,0x1C3},{0x1CD,0x1F0},{0x1F4,0x1F5},
  {0x1FA,0x217},{0x250,0x2A8},{0x2BB,0x2C1},{0x2D0,0x2D1},{0x300,0x345},
  {0x360,0x361},{0x386,0x38A},{0x38C,0x38C},{0x38E,0x3A1},{0x3A3,0x3CE},
  {0x3D0,0x3D6},{0x3DA,0x3DA},{0x3DC,0x3DC},{0x3DE,0x3DE},{0x3E0,0x3E0},
  {0x3E2,0x3F3},{0x401,0x40C},{0x40E,0x44F},{0x451,0x45C},{0x45E,0x481},
  {0x483,0x486},{0x490,0x4C4},{0x4C7,0x4C8},{0x4CB,0x4CC},{0x4D0,0x4EB},
  {0x4EE,0x4F5},{0x4F8,0x4F9},{0x531,0x556},{0x559,0x559},{0x561,0x586},
  {0x591,0x5A1},{0x5A3,0x5B9},{0x5BB,0x5BD},{0x5BF,0x5BF},{0x5C1,0x5C2},
  {0x5C4,0x5C4},{0x5D0,0x5EA},{0x5F0,0x5F2},{0x621,0x63A},{0x640,0x652},
  {0x660,0x669},{0x670,0x6B7},{0x6BA,0x6BE},{0x6C0,0x6CE},{0x6D0,0x6D3},
  {0x6D5,0x6E8},{0x6EA,0x6ED},{0x6F0,0x6F9},{0x901,0x903},{0x905,0x939},
  {0x93C,0x94D},{0x951,0x954},{0x958,0x963},{0x966,0x96F},{0x981,0x983},
  {0x985,0x98C},{0x98F,0x990},{0x993,0x9A8},{0x9AA,0x9B0},{0x9B2,0x9B2},
  {0x9B6,0x9B9},{0x9BC,0x9BC},{0x9BE,0x9C4},{0x9C7,0x9C8},{0x9CB,0x9CD},
  {0x9D7,0x9D7},{0x9DC,0x9DD},{0x9DF,0x9E3},{0x9E6,0x9F1},{0xA02,0xA02},
  {0xA05,0xA0A},{0xA0F,0xA10},{0xA13,0xA28},{0xA2A,0xA30},{0xA32,0xA33},
  {0xA35,0xA36},{0xA38,0xA39},{0xA3C,0xA3C},{0xA3E,0xA42},{0xA47,0xA48},
  {0xA4B,0xA4D},{0xA59,0xA5C},{0xA5E,0xA5E},{0xA66,0xA74},{0xA81,0xA83},
  {0xA85,0xA8B},{0xA8D,0xA8D},{0xA8F,0xA91},{0xA93,0xAA8},{0xAAA,0xAB0},
  {0xAB2,0xAB3},{0xAB5,0xAB9},{0xABC,0xAC5},{0xAC7,0xAC9},{0xACB,0xACD},
  {0xAE0,0xAE0},{0xAE6,0xAEF},{0xB01,0xB03},{0xB05,0xB0C},{0xB0F,0xB10},
  {0xB13,0xB28},{0xB2A,0xB30},{0xB32,0xB33},{0xB36,0xB39},{0xB3C,0xB43},
  {0xB47,0xB48},{0xB4B,0xB4D},{0xB56,0xB57},{0xB5C,0xB5D},{0xB5F,0xB61},
  {0xB66,0xB6F},{0xB82,0xB83},{0xB85,0xB8A},{0xB8E,0xB90},{0xB92,0xB95},
  {0xB99,0xB9A},{0xB9C,0xB9C},{0xB9E,0xB9F},{0xBA3,0xBA4},{0xBA8,0xBAA},
  {0xBAE,0xBB5},{0xBB7,0xBB9},{0xBBE,0xBC2},{0xBC6,0xBC8},{0xBCA,0xBCD},
  {0xBD7,0xBD7},{0xBE7,0xBEF},{0xC01,0xC03},{0xC05,0xC0C},{0xC0E,0xC10},
  {0xC12,0xC28},{0xC2A,0xC33},{0xC35,0xC39},{0xC3E,0xC44},{0xC46,0xC48},
  {0xC4A,0xC4D},{0xC55,0xC56},{0xC60,0xC61},{0xC66,0xC6F},{0xC82,0xC83},
  {0xC85,0xC8C},{0xC8E,0xC90},{0xC92,0xCA8},{0xCAA,0xCB3},{0xCB5,0xCB9},
  {0xCBE,0xCC4},{0xCC6,0xCC8},{0xCCA,0xCCD},{0xCD5,0xCD6},{0xCDE,0xCDE},
  {0xCE0,0xCE1},{0xCE6,0xCEF},{0xD02,0xD03},{0xD05,0xD0C},{0xD0E,0xD10},
  {0xD12,0xD28},{0xD2A,0xD39},{0xD3E,0xD43},{0xD46,0xD48},{0xD4A,0xD4D},
  {0xD57,0xD57},{0xD60,0xD61},{0xD66,0xD6F},{0xE01,0xE2E},{0xE30,0xE3A},
  {0xE40,0xE4E},{0xE50,0xE59},{0xE81,0xE82},{0xE84,0xE84},{0xE87,0xE88},
  {0xE8A,0xE8A},{0xE8D,0xE8D},{0xE94,0xE97},{0xE99,0xE9F},{0xEA1,0xEA3},
  {0xEA5,0xEA5},{0xEA7,0xEA7},{0xEAA,0xEAB},{0xEAD,0xEAE},{0xEB0,0xEB9},
  {0xEBB,0xEBD},{0xEC0,0xEC4},{0xEC6,0xEC6},{0xEC8,0xECD},{0xED0,0xED9},
  {0xF18,0xF19},{0xF20,0xF29},{0xF35,0xF35},{0xF37,0xF37},{0xF39,0xF39},
  {0xF3E,0xF47},{0xF49,0xF69},{0xF71,0xF84},{0xF86,0xF8B},{0xF90,0xF95},
  {0xF97,0xF97},{0xF99,0xFAD},{0xFB1,0xFB7},{0xFB9,0xFB9},{0x10A0,0x10C5},
  {0x10D0,0x10F6},{0x1100,0x1100},{0x1102,0x1103},{0x1105,0x1107},
  {0x1109,0x1109},{0x110B,0x110C},{0x110E,0x1112},{0x113C,0x113C},
  {0x113E,0x113E},{0x1140,0x1140},{0x114C,0x114C},{0x114E,0x114E},
  {0x1150,0x1150},{0x1154,0x1155},{0x1159,0x1159},{0x115F,0x1161},
  {0x1163,0x1163},{0x1165,0x1165},{0x1167,0x1167},{0x1169,0x1169},
  {0x116D,0x116E},{0x1172,0x1173},{0x1175,0x1175},{0x119E,0x119E},
  {0x11A8,0x11A8},{0x11AB,0x11AB},{0x11AE,0x11AF},{0x11B7,0x11B8},
  {0x11BA,0x11BA},{0x11BC,0x11C2},{0x11EB,0x11EB},{0x11F0,0x11F0},
  {0x11F9,0x11F9},{0x1E00,0x1E9B},{0x1EA0,0x1EF9},{0x1F00,0x1F15},
  {0x1F18,0x1F1D},{0x1F20,0x1F45},{0x1F48,0x1F4D},{0x1F50,0x1F57},
  {0x1F59,0x1F59},{0x1F5B,0x1F5B},{0x1F5D,0x1F5D},{0x1F5F,0x1F7D},
  {0x1F80,0x1FB4},{0x1FB6,0x1FBC},{0x1FBE,0x1FBE},{0x1FC2,0x1FC4},
  {0x1FC6,0x1FCC},{0x1FD0,0x1FD3},{0x1FD6,0x1FDB},{0x1FE0,0x1FEC},
  {0x1FF2,0x1FF4},{0x1FF6,0x1FFC},{0x20D0,0x20DC},{0x20E1,0x20E1},
  {0x2126,0x2126},{0x212A,0x212B},{0x212E,0x212E},{0x2180,0x2182},
  {0x3005,0x3005},{0x3007,0x3007},{0x3021,0x302F},{0x3031,0x3035},
  {0x3041,0x3094},{0x3099,0x309A},{0x309D,0x309E},{0x30A1,0x30FA},
  {0x30FC,0x30FE},{0x3105,0x312C},{0x4E00,0x9FA5},{0xAC00,0xD7A3},
};

static const unsigned short nm16_start[][2] = {
  {0xAA,0xAA},{0xB5,0xB5},{0xBA,0xBA},{0xC0,0xD6},{0xD8,0xF6},{0xF8,0x131},
  {0x134,0x13E},{0x141,0x148},{0x14A,0x17E},{0x180,0x1C3},{0x1CD,0x1F0},
  {0x1F4,0x1F5},{0x1FA,0x217},{0x250,0x2A8},{0x2BB,0x2C1},{0x386,0x386},
  {0x388,0x38A},{0x38C,0x38C},{0x38E,0x3A1},{0x3A3,0x3CE},{0x3D0,0x3D6},
  {0x3DA,0x3DA},{0x3DC,0x3DC},{0x3DE,0x3DE},{0x3E0,0x3E0},{0x3E2,0x3F3},
  {0x401,0x40C},{0x40E,0x44F},{0x451,0x45C},{0x45E,0x481},{0x490,0x4C4},
  {0x4C7,0x4C8},{0x4CB,0x4CC},{0x4D0,0x4EB},{0x4EE,0x4F5},{0x4F8,0x4F9},
  {0x531,0x556},{0x559,0x559},{0x561,0x586},{0x5D0,0x5EA},{0x5F0,0x5F2},
  {0x621,0x63A},{0x641,0x64A},{0x671,0x6B7},{0x6BA,0x6BE},{0x6C0,0x6CE},
  {0x6D0,0x6D3},{0x6D5,0x6D5},{0x6E5,0x6E6},{0x905,0x939},{0x93D,0x93D},
  {0x958,0x961},{0x985,0x98C},{0x98F,0x990},{0x993,0x9A8},{0x9AA,0x9B0},
  {0x9B2,0x9B2},{0x9B6,0x9B9},{0x9DC,0x9DD},{0x9DF,0x9E1},{0x9F0,0x9F1},
  {0xA05,0xA0A},{0xA0F,0xA10},{0xA13,0xA28},{0xA2A,0xA30},{0xA32,0xA33},
  {0xA35,0xA36},{0xA38,0xA39},{0xA59,0xA5C},{0xA5E,0xA5E},{0xA72,0xA74},
  {0xA85,0xA8B},{0xA8D,0xA8D},{0xA8F,0xA91},{0xA93,0xAA8},{0xAAA,0xAB0},
  {0xAB2,0xAB3},{0xAB5,0xAB9},{0xABD,0xABD},{0xAE0,0xAE0},{0xB05,0xB0C},
  {0xB0F,0xB10},{0xB13,0xB28},{0xB2A,0xB30},{0xB32,0xB33},{0xB36,0xB39},
  {0xB3D,0xB3D},{0xB5C,0xB5D},{0xB5F,0xB61},{0xB85,0xB8A},{0xB8E,0xB90},
  {0xB92,0xB95},{0xB99,0xB9A},{0xB9C,0xB9C},{0xB9E,0xB9F},{0xBA3,0xBA4},
  {0xBA8,0xBAA},{0xBAE,0xBB5},{0xBB7,0xBB9},{0xC05,0xC0C},{0xC0E,0xC10},
  {0xC12,0xC28},{0xC2A,0xC33},{0xC35,0xC39},{0xC60,0xC61},{0xC85,0xC8C},
  {0xC8E,0xC90},{0xC92,0xCA8},{0xCAA,0xCB3},{0xCB5,0xCB9},{0xCDE,0xCDE},
  {0xCE0,0xCE1},{0xD05,0xD0C},{0xD0E,0xD10},{0xD12,0xD28},{0xD2A,0xD39},
  {0xD60,0xD61},{0xE01,0xE2E},{0xE30,0xE30},{0xE32,0xE33},{0xE40,0xE45},
  {0xE81,0xE82},{0xE84,0xE84},{0xE87,0xE88},{0xE8A,0xE8A},{0xE8D,0xE8D},
  {0xE94,0xE97},{0xE99,0xE9F},{0xEA1,0xEA3},{0xEA5,0xEA5},{0xEA7,0xEA7},
  {0xEAA,0xEAB},{0xEAD,0xEAE},{0xEB0,0xEB0},{0xEB2,0xEB3},{0xEBD,0xEBD},
  {0xEC0,0xEC4},{0xF40,0xF47},{0xF49,0xF69},{0x10A0,0x10C5},{0x10D0,0x10F6},
  {0x1100,0x1100},{0x1102,0x1103},{0x1105,0x1107},{0x1109,0x1109},
  {0x110B,0x110C},{0x110E,0x1112},{0x113C,0x113C},{0x113E,0x113E},
  {0x1140,0x1140},{0x114C,0x114C},{0x114E,0x114E},{0x1150,0x1150},
  {0x1154,0x1155},{0x1159,0x1159},{0x115F,0x1161},{0x1163,0x1163},
  {0x1165,0x1165},{0x1167,0x1167},{0x1169,0x1169},{0x116D,0x116E},
  {0x1172,0x1173},{0x1175,0x1175},{0x119E,0x119E},{0x11A8,0x11A8},
  {0x11AB,0x11AB},{0x11AE,0x11AF},{0x11B7,0x11B8},{0x11BA,0x11BA},
  {0x11BC,0x11C2},{0x11EB,0x11EB},{0x11F0,0x11F0},{0x11F9,0x11F9},
  {0x1E00,0x1E9B},{0x1EA0,0x1EF9},{0x1F00,0x1F15},{0x1F18,0x1F1D},
  {0x1F20,0x1F45},{0x1F48,0x1F4D},{0x1F50,0x1F57},{0x1F59,0x1F59},
  {0x1F5B,0x1F5B},{0x1F5D,0x1F5D},{0x1F5F,0x1F7D},{0x1F80,0x1FB4},
  {0x1FB6,0x1FBC},{0x1FBE,0x1FBE},{0x1FC2,0x1FC4},{0x1FC6,0x1FCC},
  {0x1FD0,0x1FD3},{0x1FD6,0x1FDB},{0x1FE0,0x1FEC},{0x1FF2,0x1FF4},
  {0x1FF6,0x1FFC},{0x2126,0x2126},{0x212A,0x212B},{0x212E,0x212E},
  {0x2180,0x2182},{0x3007,0x3007},{0x3021,0x3029},{0x3041,0x3094},
  {0x30A1,0x30FA},{0x3105,0x312C},{0x4E00,0x9FA5},{0xAC00,0xD7A3},
};
static const unsigned short nm16_cont[][2] = {
  {0xAA,0xAA},{0xB5,0xB5},{0xB7,0xB7},{0xBA,0xBA},{0xC0,0xD6},{0xD8,0xF6},
  {0xF8,0x131},{0x134,0x13E},{0x141,0x148},{0x14A,0x17E},{0x180,0x1C3},
  {0x1CD,0x1F0},{0x1F4,0x1F5},{0x1FA,0x217},{0x250,0x2A8},{0x2BB,0x2C1},
  {0x2D0,0x2D1},{0x300,0x345},{0x360,0x361},{0x386,0x38A},{0x38C,0x38C},
  {0x38E,0x3A1},{0x3A3,0x3CE},{0x3D0,0x3D6},{0x3DA,0x3DA},{0x3DC,0x3DC},
  {0x3DE,0x3DE},{0x3E0,0x3E0},{0x3E2,0x3F3},{0x401,0x40C},{0x40E,0x44F},
  {0x451,0x45C},{0x45E,0x481},{0x483,0x486},{0x490,0x4C4},{0x4C7,0x4C8},
  {0x4CB,0x4CC},{0x4D0,0x4EB},{0x4EE,0x4F5},{0x4F8,0x4F9},{0x531,0x556},
  {0x559,0x559},{0x561,0x586},{0x591,0x5A1},{0x5A3,0x5B9},{0x5BB,0x5BD},
  {0x5BF,0x5BF},{0x5C1,0x5C2},{0x5C4,0x5C4},{0x5D0,0x5EA},{0x5F0,0x5F2},
  {0x621,0x63A},{0x640,0x652},{0x660,0x669},{0x670,0x6B7},{0x6BA,0x6BE},
  {0x6C0,0x6CE},{0x6D0,0x6D3},{0x6D5,0x6E8},{0x6EA,0x6ED},{0x6F0,0x6F9},
  {0x901,0x903},{0x905,0x939},{0x93C,0x94D},{0x951,0x954},{0x958,0x963},
  {0x966,0x96F},{0x981,0x983},{0x985,0x98C},{0x98F,0x990},{0x993,0x9A8},
  {0x9AA,0x9B0},{0x9B2,0x9B2},{0x9B6,0x9B9},{0x9BC,0x9BC},{0x9BE,0x9C4},
  {0x9C7,0x9C8},{0x9CB,0x9CD},{0x9D7,0x9D7},{0x9DC,0x9DD},{0x9DF,0x9E3},
  {0x9E6,0x9F1},{0xA02,0xA02},{0xA05,0xA0A},{0xA0F,0xA10},{0xA13,0xA28},
  {0xA2A,0xA30},{0xA32,0xA33},{0xA35,0xA36},{0xA38,0xA39},{0xA3C,0xA3C},
  {0xA3E,0xA42},{0xA47,0xA48},{0xA4B,0xA4D},{0xA59,0xA5C},{0xA5E,0xA5E},
  {0xA66,0xA74},{0xA81,0xA83},{0xA85,0xA8B},{0xA8D,0xA8D},{0xA8F,0xA91},
  {0xA93,0xAA8},{0xAAA,0xAB0},{0xAB2,0xAB3},{0xAB5,0xAB9},{0xABC,0xAC5},
  {0xAC7,0xAC9},{0xACB,0xACD},{0xAE0,0xAE0},{0xAE6,0xAEF},{0xB01,0xB03},
  {0xB05,0xB0C},{0xB0F,0xB10},{0xB13,0xB28},{0xB2A,0xB30},{0xB32,0xB33},
  {0xB36,0xB39},{0xB3C,0xB43},{0xB47,0xB48},{0xB4B,0xB4D},{0xB56,0xB57},
  {0xB5C,0xB5D},{0xB5F,0xB61},{0xB66,0xB6F},{0xB82,0xB83},{0xB85,0xB8A},
  {0xB8E,0xB90},{0xB92,0xB95},{0xB99,0xB9A},{0xB9C,0xB9C},{0xB9E,0xB9F},
  {0xBA3,0xBA4},{0xBA8,0xBAA},{0xBAE,0xBB5},{0xBB7,0xBB9},{0xBBE,0xBC2},
  {0xBC6,0xBC8},{0xBCA,0xBCD},{0xBD7,0xBD7},{0xBE7,0xBEF},{0xC01,0xC03},
  {0xC05,0xC0C},{0xC0E,0xC10},{0xC12,0xC28},{0xC2A,0xC33},{0xC35,0xC39},
  {0xC3E,0xC44},{0xC46,0xC48},{0xC4A,0xC4D},{0xC55,0xC56},{0xC60,0xC61},
  {0xC66,0xC6F},{0xC82,0xC83},{0xC85,0xC8C},{0xC8E,0xC90},{0xC92,0xCA8},
  {0xCAA,0xCB3},{0xCB5,0xCB9},{0xCBE,0xCC4},{0xCC6,0xCC8},{0xCCA,0xCCD},
  {0xCD5,0xCD6},{0xCDE,0xCDE},{0xCE0,0xCE1},{0xCE6,0xCEF},{0xD02,0xD03},
  {0xD05,0xD0C},{0xD0E,0xD10},{0xD12,0xD28},{0xD2A,0xD39},{0xD3E,0xD43},
  {0xD46,0xD48},{0xD4A,0xD4D},{0xD57,0xD57},{0xD60,0xD61},{0xD66,0xD6F},
  {0xE01,0xE2E},{0xE30,0xE3A},{0xE40,0xE4E},{0xE50,0xE59},{0xE81,0xE82},
  {0xE84,0xE84},{0xE87,0xE88},{0xE8A,0xE8A},{0xE8D,0xE8D},{0xE94,0xE97},
  {0xE99,0xE9F},{0xEA1,0xEA3},{0xEA5,0xEA5},{0xEA7,0xEA7},{0xEAA,0xEAB},
  {0xEAD,0xEAE},{0xEB0,0xEB9},{0xEBB,0xEBD},{0xEC0,0xEC4},{0xEC6,0xEC6},
  {0xEC8,0xECD},{0xED0,0xED9},{0xF18,0xF19},{0xF20,0xF29},{0xF35,0xF35},
  {0xF37,0xF37},{0xF39,0xF39},{0xF3E,0xF47},{0xF49,0xF69},{0xF71,0xF84},
  {0xF86,0xF8B},{0xF90,0xF95},{0xF97,0xF97},{0xF99,0xFAD},{0xFB1,0xFB7},
  {0xFB9,0xFB9},{0x10A0,0x10C5},{0x10D0,0x10F6},{0x1100,0x1100},
  {0x1102,0x1103},{0x1105,0x1107},{0x1109,0x1109},{0x110B,0x110C},
  {0x110E,0x1112},{0x113C,0x113C},{0x113E,0x113E},{0x1140,0x1140},
  {0x114C,0x114C},{0x114E,0x114E},{0x1150,0x1150},{0x1154,0x1155},
  {0x1159,0x1159},{0x115F,0x1161},{0x1163,0x1163},{0x1165,0x1165},
  {0x1167,0x1167},{0x1169,0x1169},{0x116D,0x116E},{0x1172,0x1173},
  {0x1175,0x1175},{0x119E,0x119E},{0x11A8,0x11A8},{0x11AB,0x11AB},
  {0x11AE,0x11AF},{0x11B7,0x11B8},{0x11BA,0x11BA},{0x11BC,0x11C2},
  {0x11EB,0x11EB},{0x11F0,0x11F0},{0x11F9,0x11F9},{0x1E00,0x1E9B},
  {0x1EA0,0x1EF9},{0x1F00,0x1F15},{0x1F18,0x1F1D},{0x1F20,0x1F45},
  {0x1F48,0x1F4D},{0x1F50,0x1F57},{0x1F59,0x1F59},{0x1F5B,0x1F5B},
  {0x1F5D,0x1F5D},{0x1F5F,0x1F7D},{0x1F80,0x1FB4},{0x1FB6,0x1FBC},
  {0x1FBE,0x1FBE},{0x1FC2,0x1FC4},{0x1FC6,0x1FCC},{0x1FD0,0x1FD3},
  {0x1FD6,0x1FDB},{0x1FE0,0x1FEC},{0x1FF2,0x1FF4},{0x1FF6,0x1FFC},
  {0x20D0,0x20DC},{0x20E1,0x20E1},{0x2126,0x2126},{0x212A,0x212B},
  {0x212E,0x212E},{0x2180,0x2182},{0x3005,0x3005},{0x3007,0x3007},
  {0x3021,0x302F},{0x3031,0x3035},{0x3041,0x3094},{0x3099,0x309A},
  {0x309D,0x309E},{0x30A1,0x30FA},{0x30FC,0x30FE},{0x3105,0x312C},
  {0x4E00,0x9FA5},{0xAC00,0xD7A3},
};

#define NM_START_N (sizeof(nm_start) / sizeof(nm_start[0]))
#define NM_CONT_N (sizeof(nm_cont) / sizeof(nm_cont[0]))
#define NM16_START_N (sizeof(nm16_start) / sizeof(nm16_start[0]))
#define NM16_CONT_N (sizeof(nm16_cont) / sizeof(nm16_cont[0]))

static int nm_in(const unsigned short (*t)[2], size_t n, unsigned long cp) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (cp < t[mid][0])
      hi = mid;
    else if (cp > t[mid][1])
      lo = mid + 1;
    else
      return 1;
  }
  return 0;
}

static int name_start(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
         || c == ':' || c >= 0x80;
}

static int name_char(unsigned char c) {
  return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'
         || c == 0xB7;
}

static int ws(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void adv(struct XML_ParserStruct *p, size_t n) {
  size_t e = p->pos + n;
  if (p->in_entity)
    p->indirect += n;
  else
    p->direct += n;
  if (p->fixed_line) {
    p->pos = e;
    p->skip = 0;
    return;
  }
  while (p->pos < e) {
    unsigned char c = p->buf[p->pos++];
    if (c == '\r') {
      p->line++;
      p->skip = 1;
    } else if (c == '\n') {
      if (p->skip)
        p->skip = 0;
      else
        p->line++;
    } else
      p->skip = 0;
  }
}

static void snap(struct XML_ParserStruct *p, Snap *s) {
  s->pos = p->pos;
  s->line = p->line;
  s->skip = p->skip;
  s->worklen = p->worklen;
  s->nav = p->nav;
  s->direct = p->direct;
  s->indirect = p->indirect;
}

static void restore(struct XML_ParserStruct *p, const Snap *s) {
  p->pos = s->pos;
  p->line = s->line;
  p->skip = s->skip;
  p->worklen = s->worklen;
  p->nav = s->nav;
  p->direct = s->direct;
  p->indirect = s->indirect;
}

static int u8(struct XML_ParserStruct *p, size_t i, size_t *n) {
  unsigned char c = p->buf[i];
  size_t rem = p->len - i;
  size_t need;
  unsigned char lo = 0x80, hi = 0xBF;
  if (c < 0x80) {
    *n = 1;
    return 1;
  }
  if (c >= 0xC2 && c <= 0xDF)
    need = 1;
  else if (c == 0xE0) {
    need = 2;
    lo = 0xA0;
  } else if (c >= 0xE1 && c <= 0xEC)
    need = 2;
  else if (c == 0xED) {
    need = 2;
    hi = 0x9F;
  } else if (c == 0xEE || c == 0xEF)
    need = 2;
  else if (c == 0xF0) {
    need = 3;
    lo = 0x90;
  } else if (c >= 0xF1 && c <= 0xF3)
    need = 3;
  else if (c == 0xF4) {
    need = 3;
    hi = 0x8F;
  } else
    return 0;
  if (rem < need + 1)
    return -1;
  if (p->buf[i + 1] < lo || p->buf[i + 1] > hi)
    return 0;
  for (size_t k = 2; k <= need; k++)
    if ((p->buf[i + k] & 0xC0) != 0x80)
      return 0;
  if (c == 0xEF && p->buf[i + 1] == 0xBF && p->buf[i + 2] >= 0xBE)
    return 0;
  *n = need + 1;
  return 1;
}

static int wreserve(struct XML_ParserStruct *p, size_t n) {
  if (p->worklen + n <= p->workcap)
    return 0;
  size_t c = p->workcap ? p->workcap : 256;
  while (c < p->worklen + n) {
    if (c > SIZE_MAX / 2)
      return -1;
    c *= 2;
  }
  char *q = realloc(p->work, c);
  if (!q)
    return -1;
  p->work = q;
  p->workcap = c;
  return 0;
}

static int wput(struct XML_ParserStruct *p, const void *s, size_t n) {
  if (wreserve(p, n))
    return -1;
  memcpy(p->work + p->worklen, s, n);
  p->worklen += n;
  return 0;
}

static int wch(struct XML_ParserStruct *p, unsigned char c) {
  if (wreserve(p, 1))
    return -1;
  p->work[p->worklen++] = (char)c;
  return 0;
}

static int wend(struct XML_ParserStruct *p) { return wch(p, 0); }

static int wcp(struct XML_ParserStruct *p, unsigned long v) {
  unsigned char b[4];
  size_t n;
  if (v < 0x80) {
    b[0] = (unsigned char)v;
    n = 1;
  } else if (v < 0x800) {
    b[0] = (unsigned char)(0xC0 | (v >> 6));
    b[1] = (unsigned char)(0x80 | (v & 0x3F));
    n = 2;
  } else if (v < 0x10000) {
    b[0] = (unsigned char)(0xE0 | (v >> 12));
    b[1] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
    b[2] = (unsigned char)(0x80 | (v & 0x3F));
    n = 3;
  } else {
    b[0] = (unsigned char)(0xF0 | (v >> 18));
    b[1] = (unsigned char)(0x80 | ((v >> 12) & 0x3F));
    b[2] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
    b[3] = (unsigned char)(0x80 | (v & 0x3F));
    n = 4;
  }
  return wput(p, b, n);
}

static int emit_chars(struct XML_ParserStruct *p, const char *s, size_t n,
                      XML_Size line) {
  if (!p->chars || n == 0)
    return 0;
  XML_Size saved = p->line;
  p->line = line;
  p->chars(p->ud, s, (int)n);
  p->line = saved;
  return 0;
}

static int emit_byte(struct XML_ParserStruct *p, unsigned char c,
                     XML_Size line) {
  char b = (char)c;
  return emit_chars(p, &b, 1, line);
}

static int emit_cp(struct XML_ParserStruct *p, unsigned long v, XML_Size line) {
  unsigned char b[4];
  size_t n;
  if (v < 0x80) {
    b[0] = (unsigned char)v;
    n = 1;
  } else if (v < 0x800) {
    b[0] = (unsigned char)(0xC0 | (v >> 6));
    b[1] = (unsigned char)(0x80 | (v & 0x3F));
    n = 2;
  } else if (v < 0x10000) {
    b[0] = (unsigned char)(0xE0 | (v >> 12));
    b[1] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
    b[2] = (unsigned char)(0x80 | (v & 0x3F));
    n = 3;
  } else {
    b[0] = (unsigned char)(0xF0 | (v >> 18));
    b[1] = (unsigned char)(0x80 | ((v >> 12) & 0x3F));
    b[2] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
    b[3] = (unsigned char)(0x80 | (v & 0x3F));
    n = 4;
  }
  return emit_chars(p, (const char *)b, n, line);
}

static int pref(struct XML_ParserStruct *p, const char *s) {
  size_t n = strlen(s), rem = p->len - p->pos;
  if (rem >= n)
    return memcmp(p->buf + p->pos, s, n) == 0 ? 1 : 0;
  return memcmp(p->buf + p->pos, s, rem) == 0 ? 2 : 0;
}

static int skip_ws(struct XML_ParserStruct *p) {
  int any = 0;
  while (p->pos < p->len && ws(p->buf[p->pos])) {
    adv(p, 1);
    any = 1;
  }
  return any;
}

static int cp_decode(struct XML_ParserStruct *p, size_t i, unsigned long *cp,
                     size_t *n) {
  unsigned char c = p->buf[i];
  if (c < 0x80) {
    *cp = c;
    *n = 1;
    return 1;
  }
  {
    int r = u8(p, i, n);
    if (r <= 0)
      return r;
  }
  if (p->buf[i] < 0xE0)
    *cp = ((unsigned long)(p->buf[i] & 0x1F) << 6)
          | (p->buf[i + 1] & 0x3F);
  else if (p->buf[i] < 0xF0)
    *cp = ((unsigned long)(p->buf[i] & 0x0F) << 12)
          | ((unsigned long)(p->buf[i + 1] & 0x3F) << 6)
          | (p->buf[i + 2] & 0x3F);
  else
    *cp = ((unsigned long)(p->buf[i] & 0x07) << 18)
          | ((unsigned long)(p->buf[i + 1] & 0x3F) << 12)
          | ((unsigned long)(p->buf[i + 2] & 0x3F) << 6)
          | (p->buf[i + 3] & 0x3F);
  return 1;
}

static int cp_name(struct XML_ParserStruct *p, unsigned long cp, int start) {
  if (cp < 0x80)
    return start ? ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')
                    || cp == '_' || cp == ':')
                 : ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')
                    || (cp >= '0' && cp <= '9') || cp == '_' || cp == ':'
                    || cp == '-' || cp == '.');
  if (p->enc == 2 || p->enc == 3)
    return start ? nm_in(nm16_start, NM16_START_N, cp)
                 : nm_in(nm16_cont, NM16_CONT_N, cp);
  return start ? nm_in(nm_start, NM_START_N, cp)
               : nm_in(nm_cont, NM_CONT_N, cp);
}

static int name_delim(struct XML_ParserStruct *p, size_t j) {
  unsigned long cp;
  size_t n;
  int r = cp_decode(p, j, &cp, &n);
  if (r <= 0)
    return 0;
  return !cp_name(p, cp, 0);
}

static int name_len(struct XML_ParserStruct *p, size_t *out) {
  size_t s = p->pos;
  int first = 1;
  for (;;) {
    unsigned long cp;
    size_t n;
    int r;
    if (p->pos == p->len)
      return 1;
    r = cp_decode(p, p->pos, &cp, &n);
    if (r < 0)
      return 2;
    if (r == 0) {
      size_t bad = p->pos;
      XML_Size badline = p->line;
      size_t j = bad + 1;
      for (;;) {
        if (j >= p->len) {
          if (p->final) {
            p->line = badline;
            return -1;
          }
          return 1;
        }
        r = cp_decode(p, j, &cp, &n);
        if (r < 0) {
          if (p->final) {
            p->line = badline;
            return -1;
          }
          return 1;
        }
        if (r == 0 || name_delim(p, j)) {
          p->line = badline;
          return -1;
        }
        j += n;
      }
    }
    if (!cp_name(p, cp, first))
      return first ? -1 : (*out = p->pos - s, 0);
    adv(p, n);
    first = 0;
  }
}

static int need_more(struct XML_ParserStruct *p, const Snap *s,
                     XML_Size tline) {
  if (p->final)
    return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
  restore(p, s);
  p->wait = 1;
  return R_WAIT;
}

static int av_reserve(struct XML_ParserStruct *p) {
  if (p->nav < p->avcap)
    return 0;
  size_t c = p->avcap ? p->avcap * 2 : 8;
  Attr *q = realloc(p->av, c * sizeof(*q));
  if (!q)
    return -1;
  p->av = q;
  p->avcap = c;
  return 0;
}

static int xml_char(unsigned long v) {
  return v == 0x9 || v == 0xA || v == 0xD || (v >= 0x20 && v <= 0xD7FF)
         || (v >= 0xE000 && v <= 0xFFFD) || (v >= 0x10000 && v <= 0x10FFFF);
}

static int content(struct XML_ParserStruct *p);
static Ent *ent_find(struct XML_ParserStruct *p, const char *s, size_t n);

static int ent_txt_u8(const char *s, size_t len, size_t i, size_t *n) {
  unsigned char c = (unsigned char)s[i];
  size_t need;
  if (c < 0x80) {
    *n = 1;
    return 1;
  }
  if (c >= 0xC2 && c <= 0xDF)
    need = 1;
  else if (c >= 0xE0 && c <= 0xEF)
    need = 2;
  else if (c >= 0xF0 && c <= 0xF4)
    need = 3;
  else
    return 0;
  if (len - i < need + 1)
    return -1;
  for (size_t k = 1; k <= need; k++)
    if (((unsigned char)s[i + k] & 0xC0) != 0x80)
      return 0;
  *n = need + 1;
  return 1;
}

static int amp_check(struct XML_ParserStruct *p) {
  unsigned long long out = p->direct + p->indirect;
  if (out < 8388608ULL)
    return 0;
  if (p->direct == 0)
    return 0;
  if ((double)out / (double)p->direct > 100.0)
    return fail(p, XML_ERROR_AMPLIFICATION_LIMIT_BREACH, p->line);
  return 0;
}

static int push_ent(struct XML_ParserStruct *p, Ent *e, XML_Size refline) {
  EntFrame *f;
  if (p->efn >= 1000000ULL)
    return fail(p, XML_ERROR_AMPLIFICATION_LIMIT_BREACH, refline);
  if (p->efn == p->efcap) {
    size_t c = p->efcap ? p->efcap * 2 : 16;
    EntFrame *q = realloc(p->eframes, c * sizeof(*q));
    if (!q)
      return nomem(p);
    p->eframes = q;
    p->efcap = c;
  }
  f = &p->eframes[p->efn++];
  f->buf = p->buf;
  f->len = p->len;
  f->cap = p->cap;
  f->pos = p->pos;
  f->skip = p->skip;
  f->state = p->state;
  f->in_cdata = p->in_cdata;
  f->in_comment = p->in_comment;
  f->fixed = p->fixed_line;
  f->final = p->final;
  f->line = p->line;
  f->fixedno = p->fixed_line_no;
  f->refline = refline;
  f->base = p->depth;
  f->ent_base = p->ent_base;
  f->ent = e;
  e->open = 1;
  p->buf = (unsigned char *)e->text;
  p->len = e->textlen;
  p->cap = 0;
  p->pos = 0;
  p->skip = 0;
  p->state = STATE_CONTENT;
  p->in_cdata = 0;
  p->in_comment = 0;
  p->fixed_line = 1;
  p->fixed_line_no = refline;
  p->line = refline;
  p->ent_base = (int)p->depth;
  p->in_entity++;
  p->final = 1;
  return 0;
}

static int pop_ent(struct XML_ParserStruct *p) {
  EntFrame *f = &p->eframes[--p->efn];
  int balanced = p->depth == f->base;
  p->buf = f->buf;
  p->len = f->len;
  p->cap = f->cap;
  p->pos = f->pos;
  p->skip = f->skip;
  p->state = f->state;
  p->in_cdata = f->in_cdata;
  p->in_comment = f->in_comment;
  p->fixed_line = f->fixed;
  p->final = f->final;
  p->line = f->line;
  p->fixed_line_no = f->fixedno;
  p->ent_base = f->ent_base;
  p->in_entity--;
  f->ent->open = 0;
  if (!balanced)
    return fail(p, XML_ERROR_ASYNC_ENTITY, f->refline);
  return 0;
}

static int ref_value(struct XML_ParserStruct *p, XML_Size tline) {
  if (p->pos == p->len)
    return R_WAIT;
  if (p->buf[p->pos] == '#') {
    int hex = 0;
    unsigned long v = 0;
    int digits = 0;
    adv(p, 1);
    if (p->pos == p->len)
      return R_WAIT;
    if (p->buf[p->pos] == 'x') {
      hex = 1;
      adv(p, 1);
    }
    for (;;) {
      if (p->pos == p->len)
        return R_WAIT;
      unsigned char c = p->buf[p->pos];
      int d;
      if (c == ';')
        break;
      if (hex) {
        if (c >= '0' && c <= '9')
          d = c - '0';
        else if (c >= 'a' && c <= 'f')
          d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
          d = c - 'A' + 10;
        else
          d = -1;
      } else
        d = (c >= '0' && c <= '9') ? c - '0' : -1;
      if (d < 0)
        return fail(p, XML_ERROR_INVALID_TOKEN, tline);
      if (v <= 0x110000)
        v = v * (hex ? 16 : 10) + (unsigned long)d;
      digits++;
      adv(p, 1);
    }
    if (!digits)
      return fail(p, XML_ERROR_INVALID_TOKEN, tline);
    adv(p, 1);
    if (!xml_char(v))
      return fail(p, XML_ERROR_BAD_CHAR_REF, tline);
    if (amp_check(p))
      return -1;
    if (emit_cp(p, v, tline))
      return nomem(p);
    return R_OK;
  }
  {
    size_t s = p->pos, n;
    int r = name_len(p, &n);
    if (r > 0) {
      if (r == 2 && p->final)
        return fail(p, XML_ERROR_PARTIAL_CHAR, tline);
      return R_WAIT;
    }
    if (r < 0)
      return fail(p, XML_ERROR_INVALID_TOKEN, tline);
    if (p->pos == p->len)
      return R_WAIT;
    if (p->buf[p->pos] != ';')
      return fail(p, XML_ERROR_INVALID_TOKEN, tline);
    adv(p, 1);
    if (amp_check(p))
      return -1;
    if (n == 3 && memcmp(p->buf + s, "amp", 3) == 0)
      return emit_byte(p, '&', tline);
    if (n == 2 && memcmp(p->buf + s, "lt", 2) == 0)
      return emit_byte(p, '<', tline);
    if (n == 2 && memcmp(p->buf + s, "gt", 2) == 0)
      return emit_byte(p, '>', tline);
    if (n == 4 && memcmp(p->buf + s, "apos", 4) == 0)
      return emit_byte(p, '\'', tline);
    if (n == 4 && memcmp(p->buf + s, "quot", 4) == 0)
      return emit_byte(p, '"', tline);
    {
      Ent *e = ent_find(p, (const char *)p->buf + s, n);
      if (!e || e->parameter) {
        if (p->has_param || p->has_ext)
          return R_OK;
        return fail(p, XML_ERROR_UNDEFINED_ENTITY, tline);
      }
      if (e->open)
        return fail(p, XML_ERROR_RECURSIVE_ENTITY_REF, tline);
      if (e->unparsed)
        return fail(p, XML_ERROR_BINARY_ENTITY_REF, tline);
      if (e->external)
        return R_OK;
      if (e->textlen == 0)
        return R_OK;
      if (amp_check(p))
        return -1;
      return push_ent(p, e, tline);
    }
  }
}

static int ref_text(struct XML_ParserStruct *p) {
  XML_Size tline = p->line;
  Snap s;
  int r;
  snap(p, &s);
  adv(p, 1);
  r = ref_value(p, tline);
  if (r == R_WAIT) {
    if (p->final)
      return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
    restore(p, &s);
    p->wait = 1;
    return R_WAIT;
  }
  return r;
}

static int text_run(struct XML_ParserStruct *p) {
  size_t s = p->pos;
  XML_Size sl = p->line;
  while (p->pos < p->len) {
    unsigned char c = p->buf[p->pos];
    if (c == '<' || c == '&')
      break;
    if (c == '\n') {
      if (amp_check(p))
        return -1;
      if (emit_chars(p, (const char *)p->buf + s, p->pos - s, sl))
        return nomem(p);
      {
        XML_Size l = p->line;
        adv(p, 1);
        if (emit_byte(p, '\n', l))
          return nomem(p);
      }
      s = p->pos;
      sl = p->line;
      continue;
    }
    if (c == '\r') {
      XML_Size l = p->line;
      if (amp_check(p))
        return -1;
      if (emit_chars(p, (const char *)p->buf + s, p->pos - s, sl))
        return nomem(p);
      adv(p, 1);
      if (p->pos < p->len && p->buf[p->pos] == '\n')
        adv(p, 1);
      if (emit_byte(p, '\n', l))
        return nomem(p);
      s = p->pos;
      sl = p->line;
      continue;
    }
    if (c == ']') {
      size_t rem = p->len - p->pos;
      if (rem >= 3 && p->buf[p->pos + 1] == ']' && p->buf[p->pos + 2] == '>')
        return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
      if (rem < 3 && !p->final) {
        p->pos = s;
        p->line = sl;
        p->wait = 1;
        return R_WAIT;
      }
      adv(p, 1);
      continue;
    }
    if (c < 0x20 && c != '\t') {
      if (emit_chars(p, (const char *)p->buf + s, p->pos - s, sl))
        return nomem(p);
      return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
    }
    if (c >= 0x80) {
      size_t n;
      int r = u8(p, p->pos, &n);
      if (r < 0) {
        if (emit_chars(p, (const char *)p->buf + s, p->pos - s, sl))
          return nomem(p);
        if (p->final)
          return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
        p->wait = 1;
        return R_WAIT;
      }
      if (r == 0) {
        if (emit_chars(p, (const char *)p->buf + s, p->pos - s, sl))
          return nomem(p);
        return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
      }
      adv(p, n);
      continue;
    }
    adv(p, 1);
  }
  if (amp_check(p))
    return -1;
  if (emit_chars(p, (const char *)p->buf + s, p->pos - s, sl))
    return nomem(p);
  return R_OK;
}

static int attr_walk(struct XML_ParserStruct *p, const char *root, size_t vl,
                     XML_Size vline, int count) {
  typedef struct {
    const char *s;
    size_t len, i;
    XML_Size line;
    int skip;
    Ent *ent;
  } AFrame;
  AFrame *fr = NULL;
  size_t n = 0, cap = 0;
  p->direct += count ? vl : 0;
  {
    cap = 8;
    fr = malloc(cap * sizeof(*fr));
    if (!fr)
      return nomem(p);
    fr[0].s = root;
    fr[0].len = vl;
    fr[0].i = 0;
    fr[0].line = vline;
    fr[0].skip = 0;
    fr[0].ent = NULL;
    n = 1;
  }
  while (n) {
    AFrame *f = &fr[n - 1];
    unsigned char c;
    if (f->i >= f->len) {
      if (f->ent) {
        f->ent->open = 0;
        n--;
        continue;
      }
      break;
    }
    if (amp_check(p)) {
      free(fr);
      return -1;
    }
    c = (unsigned char)f->s[f->i];
    if (c == '&') {
      size_t i = f->i + 1;
      if (i >= f->len) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = f->line;
        free(fr);
        return -1;
      }
      if (f->s[i] == '#') {
        int hex = 0, digits = 0;
        unsigned long v = 0;
        i++;
        if (i >= f->len) {
          p->err = XML_ERROR_INVALID_TOKEN;
          p->line = f->line;
          free(fr);
          return -1;
        }
        if (f->s[i] == 'x') {
          hex = 1;
          i++;
        }
        for (;;) {
          int d;
          if (i >= f->len) {
            p->err = XML_ERROR_INVALID_TOKEN;
            p->line = f->line;
            free(fr);
            return -1;
          }
          if (f->s[i] == ';')
            break;
          {
            unsigned char dch = (unsigned char)f->s[i];
            if (hex) {
              if (dch >= '0' && dch <= '9')
                d = dch - '0';
              else if (dch >= 'a' && dch <= 'f')
                d = dch - 'a' + 10;
              else if (dch >= 'A' && dch <= 'F')
                d = dch - 'A' + 10;
              else
                d = -1;
            } else
              d = (dch >= '0' && dch <= '9') ? dch - '0' : -1;
          }
          if (d < 0) {
            p->err = XML_ERROR_INVALID_TOKEN;
            p->line = f->line;
            free(fr);
            return -1;
          }
          if (v <= 0x110000)
            v = v * (hex ? 16 : 10) + (unsigned long)d;
          digits++;
          i++;
        }
        if (!digits) {
          p->err = XML_ERROR_INVALID_TOKEN;
          p->line = f->line;
          free(fr);
          return -1;
        }
        i++;
        if (!xml_char(v)) {
          p->err = XML_ERROR_BAD_CHAR_REF;
          p->line = f->line;
          free(fr);
          return -1;
        }
        if (wcp(p, v)) {
          free(fr);
          return nomem(p);
        }
        f->i = i;
        continue;
      }
      {
        size_t ns = i, nl;
        Ent *e;
        while (i < f->len) {
          unsigned char dch = (unsigned char)f->s[i];
          if (dch < 0x80) {
            if (!name_char(dch))
              break;
            i++;
          } else {
            size_t un;
            int ur = ent_txt_u8(f->s, f->len, i, &un);
            if (ur <= 0)
              break;
            i += un;
          }
        }
        nl = i - ns;
        if (!nl || i >= f->len || f->s[i] != ';') {
          p->err = XML_ERROR_INVALID_TOKEN;
          p->line = f->line;
          free(fr);
          return -1;
        }
        i++;
        if (nl == 3 && memcmp(f->s + ns, "amp", 3) == 0) {
          if (wch(p, '&')) {
            free(fr);
            return nomem(p);
          }
        } else if (nl == 2 && memcmp(f->s + ns, "lt", 2) == 0) {
          if (wch(p, '<')) {
            free(fr);
            return nomem(p);
          }
        } else if (nl == 2 && memcmp(f->s + ns, "gt", 2) == 0) {
          if (wch(p, '>')) {
            free(fr);
            return nomem(p);
          }
        } else if (nl == 4 && memcmp(f->s + ns, "apos", 4) == 0) {
          if (wch(p, '\'')) {
            free(fr);
            return nomem(p);
          }
        } else if (nl == 4 && memcmp(f->s + ns, "quot", 4) == 0) {
          if (wch(p, '"')) {
            free(fr);
            return nomem(p);
          }
        } else {
          e = ent_find(p, f->s + ns, nl);
          if (!e || e->parameter) {
            if (!(p->has_param || p->has_ext)) {
              p->err = XML_ERROR_UNDEFINED_ENTITY;
              p->line = f->line;
              free(fr);
              return -1;
            }
          } else if (e->open) {
            p->err = XML_ERROR_RECURSIVE_ENTITY_REF;
            p->line = f->line;
            free(fr);
            return -1;
          } else if (e->unparsed) {
            p->err = XML_ERROR_BINARY_ENTITY_REF;
            p->line = f->line;
            free(fr);
            return -1;
          } else if (e->external) {
            p->err = XML_ERROR_ATTRIBUTE_EXTERNAL_ENTITY_REF;
            p->line = f->line;
            free(fr);
            return -1;
          } else if (e->textlen) {
            if (n >= 1000000ULL) {
              p->err = XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
              p->line = f->line;
              free(fr);
              return -1;
            }
            if (n == cap) {
              AFrame *q;
              cap *= 2;
              q = realloc(fr, cap * sizeof(*fr));
              if (!q) {
                free(fr);
                return nomem(p);
              }
              fr = q;
              f = &fr[n - 1];
            }
            fr[n].s = e->text;
            fr[n].len = e->textlen;
            fr[n].i = 0;
            fr[n].line = f->line;
            fr[n].skip = 0;
            fr[n].ent = e;
            e->open = 1;
            p->indirect += e->textlen;
            n++;
            f->i = i;
            continue;
          }
        }
        f->i = i;
        continue;
      }
    }
    if (c == '<') {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = f->line;
      free(fr);
      return -1;
    }
    if (c == '\t' || c == '\n' || c == '\r') {
      if (wch(p, ' ')) {
        free(fr);
        return nomem(p);
      }
      f->i++;
      if (c == '\r') {
        if (!f->ent) {
          f->line++;
          f->skip = 1;
        }
        if (f->i < f->len && f->s[f->i] == '\n')
          f->i++;
      } else if (c == '\n') {
        if (!f->ent) {
          if (f->skip)
            f->skip = 0;
          else
            f->line++;
        }
      }
      continue;
    }
    if (!f->ent)
      f->skip = 0;
    if (c < 0x20) {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = f->line;
      free(fr);
      return -1;
    }
    if (c >= 0x80) {
      size_t un;
      int ur = ent_txt_u8(f->s, f->len, f->i, &un);
      if (ur <= 0) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = f->line;
        free(fr);
        return -1;
      }
      if (wput(p, f->s + f->i, un)) {
        free(fr);
        return nomem(p);
      }
      f->i += un;
      continue;
    }
    if (wch(p, c)) {
      free(fr);
      return nomem(p);
    }
    f->i++;
  }
  free(fr);
  return 0;
}

static ElemDef *elem_find(struct XML_ParserStruct *p, const char *s,
                          size_t n) {
  for (size_t i = 0; i < p->nedef; i++)
    if (strlen(p->edefs[i].name) == n
        && memcmp(p->edefs[i].name, s, n) == 0)
      return &p->edefs[i];
  return NULL;
}

static AttDef *attdef_find(ElemDef *e, const char *s, size_t n) {
  for (size_t i = 0; i < e->n; i++)
    if (strlen(e->attrs[i].name) == n && memcmp(e->attrs[i].name, s, n) == 0)
      return &e->attrs[i];
  return NULL;
}

static void collapse_str(char *s) {
  size_t r = 0, w = 0;
  int space = 0;
  while (s[r] == ' ')
    r++;
  for (; s[r]; r++) {
    if (s[r] == ' ') {
      if (!space)
        s[w++] = ' ';
      space = 1;
    } else {
      s[w++] = s[r];
      space = 0;
    }
  }
  while (w && s[w - 1] == ' ')
    w--;
  s[w] = 0;
}

static int av_ensure(struct XML_ParserStruct *p, size_t n) {
  if (n <= p->avcap)
    return 0;
  {
    size_t c = p->avcap ? p->avcap : 8;
    Attr *q;
    while (c < n)
      c *= 2;
    q = realloc(p->av, c * sizeof(*q));
    if (!q)
      return -1;
    p->av = q;
    p->avcap = c;
  }
  return 0;
}

static int attrs_ready(struct XML_ParserStruct *p) {
  ElemDef *ed = elem_find(p, p->work, strlen(p->work));
  size_t total = p->nav;
  for (size_t i = 0; i < p->nav; i++) {
    size_t voff = p->worklen;
    if (attr_walk(p, (const char *)p->buf + p->av[i].vs, p->av[i].vl,
                  p->av[i].vline, 1))
      return -1;
    if (wend(p))
      return -1;
    p->av[i].voff = voff;
    if (ed) {
      AttDef *a = attdef_find(ed, (const char *)p->buf + p->av[i].ns,
                              p->av[i].nl);
      if (a && a->tokenized)
        collapse_str(p->work + voff);
    }
  }
  if (ed) {
    for (size_t i = 0; i < ed->n; i++) {
      AttDef *a = &ed->attrs[i];
      int seen = 0;
      if (!a->has_dflt)
        continue;
      for (size_t j = 0; j < p->nav; j++) {
        if (p->av[j].nl == strlen(a->name)
            && memcmp(p->buf + p->av[j].ns, a->name, p->av[j].nl) == 0) {
          seen = 1;
          break;
        }
      }
      if (seen)
        continue;
      if (av_ensure(p, total + 1))
        return -1;
      {
        size_t noff = p->worklen;
        if (wput(p, a->name, strlen(a->name)) || wend(p))
          return -1;
        {
          size_t voff = p->worklen;
          if (wput(p, a->dflt, a->dlen) || wend(p))
            return -1;
          p->av[total].noff = noff;
          p->av[total].voff = voff;
          total++;
        }
      }
    }
  }
  if (p->attpcap < total * 2 + 1) {
    size_t c = p->attpcap ? p->attpcap : 16;
    while (c < total * 2 + 1)
      c *= 2;
    const char **q = realloc(p->attp, c * sizeof(*q));
    if (!q)
      return -1;
    p->attp = q;
    p->attpcap = c;
  }
  for (size_t i = 0; i < total; i++) {
    p->attp[i * 2] = p->work + p->av[i].noff;
    p->attp[i * 2 + 1] = p->work + p->av[i].voff;
  }
  p->attp[total * 2] = NULL;
  return 0;
}

static int push_name(struct XML_ParserStruct *p, const char *s, size_t n) {
  if (p->depth == p->stackcap) {
    size_t c = p->stackcap ? p->stackcap * 2 : 8;
    char **q = realloc(p->stack, c * sizeof(*q));
    if (!q)
      return -1;
    p->stack = q;
    p->stackcap = c;
  }
  {
    char *d = malloc(n + 1);
    if (!d)
      return -1;
    memcpy(d, s, n);
    d[n] = 0;
    p->stack[p->depth++] = d;
  }
  return 0;
}

static int start_done(struct XML_ParserStruct *p, XML_Size tline,
                      XML_Size eline, int empty, int depth0) {
  XML_Size saved = p->line;
  const char *name;
  if (attrs_ready(p)) {
    if (p->err == XML_ERROR_NONE)
      return nomem(p);
    return -1;
  }
  name = p->work;
  if (p->start) {
    p->line = tline;
    p->start(p->ud, name, p->attp);
  }
  if (empty) {
    if (p->end) {
      p->line = eline;
      p->end(p->ud, name);
    }
    if (depth0)
      p->state = STATE_EPILOG;
  } else {
    if (push_name(p, name, strlen(name)))
      return nomem(p);
  }
  p->line = saved;
  return R_OK;
}

static int attr(struct XML_ParserStruct *p, int *need, XML_Size tline) {
  size_t ns = p->pos, nl;
  XML_Size nline = p->line;
  int r = name_len(p, &nl);
  *need = 0;
  if (r > 0) {
    if (r == 2 && p->final)
      return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
    *need = 1;
    return R_OK;
  }
  if (r < 0)
    return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
  for (size_t i = 0; i < p->nav; i++)
    if (p->av[i].nl == nl && memcmp(p->buf + p->av[i].ns, p->buf + ns, nl) == 0)
      return fail(p, XML_ERROR_DUPLICATE_ATTRIBUTE, nline);
  if (av_reserve(p))
    return nomem(p);
  {
    size_t noff = p->worklen;
    if (wput(p, p->buf + ns, nl) || wend(p))
      return nomem(p);
    p->av[p->nav].ns = ns;
    p->av[p->nav].nl = nl;
    p->av[p->nav].noff = noff;
  }
  skip_ws(p);
  if (p->pos == p->len) {
    *need = 1;
    return R_OK;
  }
  if (p->buf[p->pos] != '=')
    return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
  adv(p, 1);
  skip_ws(p);
  if (p->pos == p->len) {
    *need = 1;
    return R_OK;
  }
  {
    unsigned char q = p->buf[p->pos];
    if (q != '"' && q != '\'')
      return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
    adv(p, 1);
    {
      size_t vs = p->pos, scan = p->pos;
      XML_Size ln = p->line, vline = p->line;
      int lskip = p->skip;
      for (;;) {
        unsigned char c;
        if (scan == p->len) {
          if (!p->final) {
            *need = 1;
            return R_OK;
          }
          return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
        }
        c = p->buf[scan];
        if (c == q)
          break;
        if (c == '<')
          return fail(p, XML_ERROR_INVALID_TOKEN, ln);
        if (c == '\r') {
          scan++;
          ln++;
          lskip = 1;
          continue;
        }
        if (c == '\n') {
          scan++;
          if (lskip)
            lskip = 0;
          else
            ln++;
          continue;
        }
        lskip = 0;
        if (c == '&') {
          size_t j = scan + 1;
          if (j >= p->len) {
            if (!p->final) {
              *need = 1;
              return R_OK;
            }
            return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
          }
          if (p->buf[j] == '#') {
            int hex = 0, digits = 0;
            j++;
            if (j >= p->len) {
              if (!p->final) {
                *need = 1;
                return R_OK;
              }
              return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
            }
            if (p->buf[j] == 'x') {
              hex = 1;
              j++;
            }
            for (;;) {
              unsigned char d;
              int ok;
              if (j >= p->len) {
                if (!p->final) {
                  *need = 1;
                  return R_OK;
                }
                return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
              }
              d = p->buf[j];
              if (d == ';') {
                j++;
                break;
              }
              if (hex)
                ok = (d >= '0' && d <= '9') || (d >= 'a' && d <= 'f')
                     || (d >= 'A' && d <= 'F');
              else
                ok = d >= '0' && d <= '9';
              if (!ok)
                return fail(p, XML_ERROR_INVALID_TOKEN, ln);
              digits++;
              j++;
            }
            if (!digits)
              return fail(p, XML_ERROR_INVALID_TOKEN, ln);
            scan = j;
            continue;
          }
          if (!name_start(p->buf[j]))
            return fail(p, XML_ERROR_INVALID_TOKEN, ln);
          {
            while (j < p->len) {
              unsigned char d = p->buf[j];
              if (d < 0x80) {
                if (!name_char(d))
                  break;
                j++;
              } else {
                size_t n;
                int ur = u8(p, j, &n);
                if (ur < 0) {
                  if (p->final)
                    return fail(p, XML_ERROR_PARTIAL_CHAR, ln);
                  *need = 1;
                  return R_OK;
                }
                if (ur == 0)
                  return fail(p, XML_ERROR_INVALID_TOKEN, ln);
                j += n;
              }
            }
          }
          if (j >= p->len) {
            if (!p->final) {
              *need = 1;
              return R_OK;
            }
            return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
          }
          if (p->buf[j] == ';')
            j++;
          scan = j;
          continue;
        }
        if (c < 0x20 && c != '\t')
          return fail(p, XML_ERROR_INVALID_TOKEN, ln);
        if (c >= 0x80) {
          size_t n;
          int ur = u8(p, scan, &n);
          if (ur < 0) {
            if (p->final)
              return fail(p, XML_ERROR_PARTIAL_CHAR, ln);
            *need = 1;
            return R_OK;
          }
          if (ur == 0)
            return fail(p, XML_ERROR_INVALID_TOKEN, ln);
          scan += n;
          continue;
        }
        scan++;
      }
      p->av[p->nav].vs = vs;
      p->av[p->nav].vl = scan - vs;
      p->av[p->nav].vline = vline;
      scan++;
      while (p->pos < scan)
        adv(p, 1);
      p->nav++;
    }
  }
  return R_OK;
}

static int start_tag(struct XML_ParserStruct *p, XML_Size tline, int depth0) {
  size_t nl;
  int r, need;
  Snap s;
  snap(p, &s);
  p->worklen = 0;
  p->nav = 0;
  adv(p, 1);
  r = name_len(p, &nl);
  if (r > 0) {
    if (r == 2 && p->final)
      return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
    return need_more(p, &s, tline);
  }
  if (r < 0)
    return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
  if (wput(p, p->buf + p->pos - nl, nl) || wend(p))
    return nomem(p);
  for (;;) {
    int had = skip_ws(p);
    if (p->pos == p->len)
      return need_more(p, &s, tline);
    if (p->buf[p->pos] == '>') {
      adv(p, 1);
      return start_done(p, tline, 0, 0, depth0);
    }
    if (p->buf[p->pos] == '/') {
      XML_Size el = p->line;
      adv(p, 1);
      if (p->pos == p->len)
        return need_more(p, &s, tline);
      if (p->buf[p->pos] != '>')
        return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
      adv(p, 1);
      return start_done(p, tline, el, 1, depth0);
    }
    if (!had)
      return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
    r = attr(p, &need, tline);
    if (r < 0)
      return -1;
    if (need)
      return need_more(p, &s, tline);
  }
}

static int end_tag(struct XML_ParserStruct *p, XML_Size tline) {
  size_t nl;
  int r;
  Snap s;
  XML_Size nline;
  snap(p, &s);
  adv(p, 2);
  if (p->pos == p->len)
    return need_more(p, &s, tline);
  nline = p->line;
  r = name_len(p, &nl);
  if (r > 0) {
    if (r == 2 && p->final)
      return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
    return need_more(p, &s, tline);
  }
  if (r < 0)
    return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
  {
    size_t ns = p->pos - nl;
    skip_ws(p);
    if (p->pos == p->len)
      return need_more(p, &s, tline);
    if (p->buf[p->pos] != '>')
      return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
    if (p->in_entity && p->depth == (size_t)p->ent_base)
      return fail(p, XML_ERROR_ASYNC_ENTITY, tline);
    if (p->depth == 0)
      return fail(p, XML_ERROR_TAG_MISMATCH, nline);
    if (strlen(p->stack[p->depth - 1]) != nl
        || memcmp(p->stack[p->depth - 1], p->buf + ns, nl) != 0)
      return fail(p, XML_ERROR_TAG_MISMATCH, nline);
  }
  adv(p, 1);
  {
    char *nm = p->stack[--p->depth];
    XML_Size saved = p->line;
    if (p->end) {
      p->line = tline;
      p->end(p->ud, nm);
    }
    p->line = saved;
    free(nm);
  }
  if (p->depth == 0)
    p->state = STATE_EPILOG;
  return R_OK;
}

static int comment_body(struct XML_ParserStruct *p, XML_Size tline) {
  for (;;) {
    if (p->pos == p->len) {
      if (p->final) {
        p->in_comment = 0;
        return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
      }
      p->in_comment = 1;
      p->cm_tline = tline;
      p->wait = 1;
      return R_OK;
    }
    {
      unsigned char c = p->buf[p->pos];
      if (c == '-') {
        size_t rem = p->len - p->pos;
        if (rem == 1) {
          if (p->final) {
            if (wch(p, '-'))
              return nomem(p);
            adv(p, 1);
            continue;
          }
          p->in_comment = 1;
          p->cm_tline = tline;
          p->wait = 1;
          return R_OK;
        }
        if (p->buf[p->pos + 1] == '-') {
          if (rem < 3) {
            if (p->final) {
              if (wch(p, '-') || wch(p, '-'))
                return nomem(p);
              adv(p, 2);
              continue;
            }
            p->in_comment = 1;
            p->cm_tline = tline;
            p->wait = 1;
            return R_OK;
          }
          if (p->buf[p->pos + 2] != '>') {
            p->in_comment = 0;
            return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
          }
          adv(p, 3);
          break;
        }
        if (wch(p, '-'))
          return nomem(p);
        adv(p, 1);
        continue;
      }
      if (c == '\n' || c == '\r') {
        if (wch(p, '\n'))
          return nomem(p);
        if (c == '\r') {
          adv(p, 1);
          if (p->pos < p->len && p->buf[p->pos] == '\n')
            adv(p, 1);
        } else
          adv(p, 1);
        continue;
      }
      if (c < 0x20 && c != '\t') {
        p->in_comment = 0;
        return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
      }
      if (c >= 0x80) {
        size_t n;
        int r = u8(p, p->pos, &n);
        if (r < 0) {
          if (p->final) {
            p->in_comment = 0;
            return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
          }
          p->in_comment = 1;
          p->cm_tline = tline;
          p->wait = 1;
          return R_OK;
        }
        if (r == 0) {
          p->in_comment = 0;
          return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
        }
        if (wput(p, p->buf + p->pos, n))
          return nomem(p);
        adv(p, n);
        continue;
      }
      if (wch(p, c))
        return nomem(p);
      adv(p, 1);
    }
  }
  p->in_comment = 0;
  if (wend(p))
    return nomem(p);
  if (p->comment) {
    XML_Size saved = p->line;
    p->line = tline;
    p->comment(p->ud, p->work);
    p->line = saved;
  }
  p->at_start = 0;
  return R_OK;
}

static int comment(struct XML_ParserStruct *p, XML_Size tline) {
  p->worklen = 0;
  adv(p, 4);
  p->in_comment = 1;
  p->cm_tline = tline;
  return comment_body(p, tline);
}

static int cdata_body(struct XML_ParserStruct *p, XML_Size tline) {
  for (;;) {
    size_t st = p->pos;
    XML_Size sl = p->line;
    while (p->pos < p->len) {
      unsigned char c = p->buf[p->pos];
      if (c == ']') {
        size_t rem = p->len - p->pos;
        if (rem >= 3 && p->buf[p->pos + 1] == ']'
            && p->buf[p->pos + 2] == '>') {
          if (emit_chars(p, (const char *)p->buf + st, p->pos - st, sl))
            return nomem(p);
          adv(p, 3);
          p->in_cdata = 0;
          return R_OK;
        }
        if (rem < 3 && !p->final) {
          if (emit_chars(p, (const char *)p->buf + st, p->pos - st, sl))
            return nomem(p);
          p->in_cdata = 1;
          p->wait = 1;
          return R_OK;
        }
        adv(p, 1);
        continue;
      }
      if (c == '\n' || c == '\r') {
        XML_Size l = p->line;
        if (emit_chars(p, (const char *)p->buf + st, p->pos - st, sl))
          return nomem(p);
        if (c == '\r') {
          adv(p, 1);
          if (p->pos < p->len && p->buf[p->pos] == '\n')
            adv(p, 1);
        } else
          adv(p, 1);
        if (emit_byte(p, '\n', l))
          return nomem(p);
        st = p->pos;
        sl = p->line;
        continue;
      }
      if (c < 0x20 && c != '\t') {
        if (emit_chars(p, (const char *)p->buf + st, p->pos - st, sl))
          return nomem(p);
        p->in_cdata = 0;
        return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
      }
      if (c >= 0x80) {
        size_t n;
        int r = u8(p, p->pos, &n);
        if (r < 0) {
          if (emit_chars(p, (const char *)p->buf + st, p->pos - st, sl))
            return nomem(p);
          if (p->final) {
            p->in_cdata = 0;
            return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
          }
          p->in_cdata = 1;
          p->wait = 1;
          return R_OK;
        }
        if (r == 0) {
          if (emit_chars(p, (const char *)p->buf + st, p->pos - st, sl))
            return nomem(p);
          p->in_cdata = 0;
          return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
        }
        adv(p, n);
        continue;
      }
      adv(p, 1);
    }
    if (p->pos == p->len) {
      size_t end = p->pos;
      size_t hold = 0;
      while (hold < 2 && end > st && p->buf[end - 1] == ']') {
        end--;
        hold++;
      }
      if (emit_chars(p, (const char *)p->buf + st, end - st, sl))
        return nomem(p);
      if (p->final) {
        p->in_cdata = 0;
        return fail(p, XML_ERROR_UNCLOSED_CDATA_SECTION, p->line);
      }
      p->in_cdata = 1;
      p->cd_tline = tline;
      return R_OK;
    }
  }
}

static int cdata(struct XML_ParserStruct *p, XML_Size tline) {
  adv(p, 9);
  p->in_cdata = 1;
  p->cd_tline = tline;
  return cdata_body(p, tline);
}

static int reserved_xml(const char *s, size_t n) {
  if (n != 3)
    return 0;
  return (s[0] == 'x' || s[0] == 'X') && (s[1] == 'm' || s[1] == 'M')
         && (s[2] == 'l' || s[2] == 'L');
}

static int decl_bad(struct XML_ParserStruct *p, const Snap *s,
                    XML_Size tline) {
  unsigned char c = p->buf[p->pos];
  if (c < 0x20) {
    if (ws(c))
      return fail(p, XML_ERROR_XML_DECL, p->line);
    return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
  }
  if (c >= 0x80) {
    size_t n;
    int r = u8(p, p->pos, &n);
    if (r < 0) {
      if (p->final)
        return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
      return need_more(p, s, tline);
    }
    if (r == 0)
      return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
    if (name_start(c))
      return fail(p, XML_ERROR_XML_DECL, tline);
    return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
  }
  if (ws(c) || name_start(c) || (c >= '0' && c <= '9'))
    return fail(p, XML_ERROR_XML_DECL, tline);
  return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
}

static int decl_value(struct XML_ParserStruct *p, size_t *s, size_t *n,
                      XML_Size tline) {
  unsigned char q;
  if (p->pos == p->len)
    return R_WAIT;
  q = p->buf[p->pos];
  if (q != '"' && q != '\'') {
    Snap z;
    snap(p, &z);
    return decl_bad(p, &z, tline);
  }
  adv(p, 1);
  *s = p->pos;
  while (p->pos < p->len && p->buf[p->pos] != q) {
    unsigned char c = p->buf[p->pos];
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
      return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
    if (c >= 0x80) {
      size_t un;
      int r = u8(p, p->pos, &un);
      if (r < 0) {
        if (p->final)
          return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
        return R_WAIT;
      }
      if (r == 0)
        return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
      adv(p, un);
      continue;
    }
    adv(p, 1);
  }
  if (p->pos == p->len)
    return R_WAIT;
  *n = p->pos - *s;
  adv(p, 1);
  return R_OK;
}

static int kw_check(struct XML_ParserStruct *p, const Snap *s,
                    XML_Size tline, const char *kw) {
  size_t n = strlen(kw);
  for (size_t i = 0; i < n; i++) {
    unsigned char c;
    if (p->pos + i >= p->len) {
      if (p->final)
        return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
      return need_more(p, s, tline);
    }
    c = p->buf[p->pos + i];
    if (c != (unsigned char)kw[i]) {
      if (c < 0x20)
        return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
      if (c >= 0x80) {
        size_t un;
        int rr = u8(p, p->pos + i, &un);
        if (rr < 0) {
          if (p->final)
            return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
          return need_more(p, s, tline);
        }
        if (rr == 0)
          return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
      }
      return fail(p, XML_ERROR_XML_DECL, tline);
    }
  }
  return R_OK;
}

static int xml_decl(struct XML_ParserStruct *p, XML_Size tline) {
  Snap s;
  int r;
  size_t vs = 0, vn = 0, es = 0, en = 0, ss = 0, sn = 0;
  int have_enc = 0, have_std = 0;
  snap(p, &s);
  adv(p, 5);
  if (p->pos == p->len)
    return need_more(p, &s, tline);
  if (!ws(p->buf[p->pos]))
    return decl_bad(p, &s, tline);
  skip_ws(p);
  if (p->pos == p->len)
    return need_more(p, &s, tline);
  r = kw_check(p, &s, tline, "version");
  if (r < 0)
    return r;
  if (r == R_WAIT)
    return R_WAIT;
  adv(p, 7);
  skip_ws(p);
  if (p->pos == p->len)
    return need_more(p, &s, tline);
  if (p->buf[p->pos] != '=')
    return decl_bad(p, &s, tline);
  adv(p, 1);
  skip_ws(p);
  r = decl_value(p, &vs, &vn, tline);
  if (r == R_WAIT) {
    if (p->final)
      return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
    return need_more(p, &s, tline);
  }
  if (r < 0)
    return -1;
  skip_ws(p);
  if (p->pos == p->len)
    return need_more(p, &s, tline);
  if (p->buf[p->pos] != '?') {
    r = kw_check(p, &s, tline, "encoding");
    if (r < 0)
      return r;
    if (r == R_WAIT)
      return R_WAIT;
    adv(p, 8);
    skip_ws(p);
    if (p->pos == p->len)
      return need_more(p, &s, tline);
    if (p->buf[p->pos] != '=')
      return decl_bad(p, &s, tline);
    adv(p, 1);
    skip_ws(p);
    r = decl_value(p, &es, &en, tline);
    if (r == R_WAIT) {
      if (p->final)
        return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
      return need_more(p, &s, tline);
    }
    if (r < 0)
      return -1;
    have_enc = 1;
    skip_ws(p);
    if (p->pos == p->len)
      return need_more(p, &s, tline);
  }
  if (p->buf[p->pos] != '?') {
    r = kw_check(p, &s, tline, "standalone");
    if (r < 0)
      return r;
    if (r == R_WAIT)
      return R_WAIT;
    adv(p, 10);
    skip_ws(p);
    if (p->pos == p->len)
      return need_more(p, &s, tline);
    if (p->buf[p->pos] != '=')
      return decl_bad(p, &s, tline);
    adv(p, 1);
    skip_ws(p);
    r = decl_value(p, &ss, &sn, tline);
    if (r == R_WAIT) {
      if (p->final)
        return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
      return need_more(p, &s, tline);
    }
    if (r < 0)
      return -1;
    have_std = 1;
    skip_ws(p);
    if (p->pos == p->len)
      return need_more(p, &s, tline);
  }
  if (p->buf[p->pos] != '?')
    return decl_bad(p, &s, tline);
  if (p->len - p->pos < 2)
    return need_more(p, &s, tline);
  if (p->buf[p->pos + 1] != '>') {
    for (size_t i = p->pos + 1; i + 1 < p->len; i++)
      if (p->buf[i] == '?' && p->buf[i + 1] == '>')
        return fail(p, XML_ERROR_XML_DECL, tline);
    if (p->final)
      return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
    return need_more(p, &s, tline);
  }
  adv(p, 2);
  {
    size_t n = vn;
    for (size_t i = 0; i < n; i++) {
      unsigned char c = p->buf[vs + i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
        return fail(p, XML_ERROR_XML_DECL, tline);
    }
  }
  if (have_enc) {
    size_t n = en;
    if (n == 0)
      return fail(p, XML_ERROR_XML_DECL, tline);
    for (size_t i = 0; i < n; i++) {
      unsigned char c = p->buf[es + i];
      if (i == 0) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
          return fail(p, XML_ERROR_XML_DECL, tline);
      } else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                   || (c >= '0' && c <= '9') || c == '.' || c == '_'
                   || c == '-'))
        return fail(p, XML_ERROR_XML_DECL, tline);
    }
  }
  if (have_std) {
    if (!((sn == 3 && memcmp(p->buf + ss, "yes", 3) == 0)
          || (sn == 2 && memcmp(p->buf + ss, "no", 2) == 0)))
      return fail(p, XML_ERROR_XML_DECL, tline);
  }
  p->at_start = 0;
  return R_OK;
}

static int pi_complete(struct XML_ParserStruct *p) {
  for (size_t i = p->pos; i + 1 < p->len; i++)
    if (p->buf[i] == '?' && p->buf[i + 1] == '>')
      return 1;
  return 0;
}

static int pi(struct XML_ParserStruct *p, XML_Size tline, int prolog) {
  Snap s;
  size_t nl;
  int r;
  {
    int x = pref(p, "<?xml");
    if (x == 2) {
      if (p->final)
        return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
      p->wait = 1;
      return R_OK;
    }
    if (x == 1) {
      size_t after = p->pos + 5;
      if (after < p->len) {
        if (!name_char(p->buf[after])) {
          if (prolog && p->at_start)
            return xml_decl(p, tline);
          if (!pi_complete(p)) {
            if (p->final)
              return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
            p->wait = 1;
            return R_OK;
          }
          return fail(p, XML_ERROR_MISPLACED_XML_PI, tline);
        }
      } else if (!p->final) {
        p->wait = 1;
        return R_OK;
      } else {
        if (prolog && p->at_start)
          return xml_decl(p, tline);
        if (!pi_complete(p)) {
          if (p->final)
            return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
          p->wait = 1;
          return R_OK;
        }
        return fail(p, XML_ERROR_MISPLACED_XML_PI, tline);
      }
    }
  }
  snap(p, &s);
  adv(p, 2);
  if (p->pos == p->len)
    return need_more(p, &s, tline);
  if (!name_start(p->buf[p->pos]))
    return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
  r = name_len(p, &nl);
  if (r > 0) {
    if (r == 2 && p->final)
      return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
    return need_more(p, &s, tline);
  }
  if (r < 0)
    return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
  {
    size_t ns = p->pos - nl;
    if (nl == 3 && reserved_xml((const char *)p->buf + ns, nl)) {
      if (!pi_complete(p)) {
        if (p->final)
          return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
        p->wait = 1;
        return R_OK;
      }
      return fail(p, XML_ERROR_INVALID_TOKEN, tline);
    }
    if (p->pos == p->len)
      return need_more(p, &s, tline);
    if (!ws(p->buf[p->pos]) && p->buf[p->pos] != '?')
      return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
    p->worklen = 0;
    if (wput(p, p->buf + ns, nl) || wend(p))
      return nomem(p);
    skip_ws(p);
    {
      size_t doff = p->worklen;
      for (;;) {
        if (p->pos == p->len)
          return need_more(p, &s, tline);
        {
          unsigned char c = p->buf[p->pos];
          if (c == '?') {
            if (p->len - p->pos < 2)
              return need_more(p, &s, tline);
            if (p->buf[p->pos + 1] == '>') {
              adv(p, 2);
              break;
            }
            if (wch(p, '?'))
              return nomem(p);
            adv(p, 1);
            continue;
          }
          if (c == '\n' || c == '\r') {
            if (wch(p, '\n'))
              return nomem(p);
            if (c == '\r') {
              adv(p, 1);
              if (p->pos < p->len && p->buf[p->pos] == '\n')
                adv(p, 1);
            } else
              adv(p, 1);
            continue;
          }
          if (c < 0x20 && c != '\t')
            return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
          if (c >= 0x80) {
            size_t n;
            int ur = u8(p, p->pos, &n);
            if (ur < 0)
              return need_more(p, &s, tline);
            if (ur == 0)
              return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
            if (wput(p, p->buf + p->pos, n))
              return nomem(p);
            adv(p, n);
            continue;
          }
          if (wch(p, c))
            return nomem(p);
          adv(p, 1);
        }
      }
      if (wend(p))
        return nomem(p);
      {
        XML_Size saved = p->line;
        p->line = tline;
        if (p->pi)
          p->pi(p->ud, p->work, p->work + doff);
        p->line = saved;
      }
    }
  }
  p->at_start = 0;
  return R_OK;
}

typedef struct {
  size_t i;
  XML_Size line;
  int skip;
} DCur;

static int dt_reserve(struct XML_ParserStruct *p, size_t n) {
  if (p->dtdlen + n <= p->dtdcap)
    return 0;
  size_t c = p->dtdcap ? p->dtdcap : 1024;
  while (c < p->dtdlen + n) {
    if (c > SIZE_MAX / 2)
      return -1;
    c *= 2;
  }
  unsigned char *q = realloc(p->dtd, c);
  if (!q)
    return -1;
  p->dtd = q;
  p->dtdcap = c;
  return 0;
}

static int dt_append(struct XML_ParserStruct *p) {
  size_t n = p->len - p->pos;
  if (!n)
    return 0;
  if (dt_reserve(p, n))
    return -1;
  memcpy(p->dtd + p->dtdlen, p->buf + p->pos, n);
  p->dtdlen += n;
  p->pos = p->len;
  return 0;
}

static int dt_u8(struct XML_ParserStruct *p, size_t i, size_t *n) {
  unsigned char c = p->dtd[i];
  size_t rem = p->dtdlen - i;
  size_t need;
  unsigned char lo = 0x80, hi = 0xBF;
  if (c < 0x80) {
    *n = 1;
    return 1;
  }
  if (c >= 0xC2 && c <= 0xDF)
    need = 1;
  else if (c == 0xE0) {
    need = 2;
    lo = 0xA0;
  } else if (c >= 0xE1 && c <= 0xEC)
    need = 2;
  else if (c == 0xED) {
    need = 2;
    hi = 0x9F;
  } else if (c == 0xEE || c == 0xEF)
    need = 2;
  else if (c == 0xF0) {
    need = 3;
    lo = 0x90;
  } else if (c >= 0xF1 && c <= 0xF3)
    need = 3;
  else if (c == 0xF4) {
    need = 3;
    hi = 0x8F;
  } else
    return 0;
  if (rem < need + 1)
    return -1;
  if (p->dtd[i + 1] < lo || p->dtd[i + 1] > hi)
    return 0;
  for (size_t k = 2; k <= need; k++)
    if ((p->dtd[i + k] & 0xC0) != 0x80)
      return 0;
  if (c == 0xEF && p->dtd[i + 1] == 0xBF && p->dtd[i + 2] >= 0xBE)
    return 0;
  *n = need + 1;
  return 1;
}

static int dt_cp(struct XML_ParserStruct *p, size_t i, size_t *n) {
  int r = dt_u8(p, i, n);
  if (r <= 0)
    return r;
  if (*n == 1)
    return p->dtd[i];
  if (*n == 2)
    return ((p->dtd[i] & 0x1F) << 6) | (p->dtd[i + 1] & 0x3F);
  if (*n == 3)
    return ((p->dtd[i] & 0x0F) << 12) | ((p->dtd[i + 1] & 0x3F) << 6)
           | (p->dtd[i + 2] & 0x3F);
  return ((p->dtd[i] & 0x07) << 18) | ((p->dtd[i + 1] & 0x3F) << 12)
         | ((p->dtd[i + 2] & 0x3F) << 6) | (p->dtd[i + 3] & 0x3F);
}

static void dv(struct XML_ParserStruct *p, DCur *c, size_t n) {
  p->direct += n;
  for (size_t k = 0; k < n && c->i < p->dtdlen; k++) {
    unsigned char ch = p->dtd[c->i++];
    if (ch == '\r') {
      c->line++;
      c->skip = 1;
    } else if (ch == '\n') {
      if (c->skip)
        c->skip = 0;
      else
        c->line++;
    } else
      c->skip = 0;
  }
}

static int dt_ws(struct XML_ParserStruct *p, DCur *c) {
  int any = 0;
  while (c->i < p->dtdlen && ws(p->dtd[c->i])) {
    dv(p, c, 1);
    any = 1;
  }
  return any;
}

static int dt_name(struct XML_ParserStruct *p, DCur *c, size_t *ns,
                   size_t *nl) {
  int first = 1;
  size_t s = c->i;
  for (;;) {
    int cp;
    size_t n;
    if (c->i >= p->dtdlen) {
      if (p->final && !first) {
        *ns = s;
        *nl = c->i - s;
        return 0;
      }
      return 1;
    }
    cp = dt_cp(p, c->i, &n);
    if (cp <= 0 || !cp_name(p, (unsigned long)cp, first)) {
      unsigned char tc;
      if (first) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c->line;
        return -1;
      }
      tc = p->dtd[c->i];
      if (!(ws(tc) || tc == '>' || tc == ')' || tc == ',' || tc == '|'
            || tc == '[' || tc == '%' || tc == '?' || tc == '*' || tc == '+'
            || tc == ';')) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c->line;
        return -3;
      }
      *nl = c->i - s;
      *ns = s;
      return 0;
    }
    dv(p, c, n);
    first = 0;
  }
}

static int dt_need(struct XML_ParserStruct *p, XML_Size line,
                   enum XML_Error finalerr) {
  if (p->final)
    return fail(p, finalerr, line);
  p->wait = 1;
  return R_OK;
}

static int dt_bad(struct XML_ParserStruct *p, DCur *c, int adjacent);

static int dt_lit2(struct XML_ParserStruct *p, DCur *c, size_t *os,
                   size_t *on, int pubid) {
  unsigned char q;
  if (c->i >= p->dtdlen)
    return 1;
  q = p->dtd[c->i];
  if (q != '"' && q != '\'')
    return dt_bad(p, c, 0);
  dv(p, c, 1);
  *os = c->i;
  while (c->i < p->dtdlen && p->dtd[c->i] != q) {
    unsigned char ch = p->dtd[c->i];
    if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c->line;
      return -1;
    }
    if (ch >= 0x80) {
      size_t n;
      int r = dt_u8(p, c->i, &n);
      if (r < 0) {
        if (p->final) {
          p->err = XML_ERROR_PARTIAL_CHAR;
          p->line = c->line;
          return -1;
        }
        return 1;
      }
      if (r == 0) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c->line;
        return -1;
      }
      dv(p, c, n);
      if (pubid) {
        p->err = XML_ERROR_PUBLICID;
        p->line = c->line;
        return -1;
      }
      continue;
    }
    if (pubid) {
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '\r'
            || ch == '\n' || ch == '-' || ch == '\'' || ch == '('
            || ch == ')' || ch == '+' || ch == ',' || ch == '.' || ch == '/'
            || ch == ':' || ch == '=' || ch == '?' || ch == ';' || ch == '!'
            || ch == '*' || ch == '#' || ch == '@' || ch == '$' || ch == '_'
            || ch == '%')) {
        p->err = XML_ERROR_PUBLICID;
        p->line = c->line;
        return -1;
      }
    }
    dv(p, c, 1);
  }
  if (c->i >= p->dtdlen)
    return 1;
  if (c->i + 1 >= p->dtdlen && !p->final) {
    *on = c->i - *os;
    return 1;
  }
  *on = c->i - *os;
  if (c->i + 1 < p->dtdlen) {
    unsigned char nx = p->dtd[c->i + 1];
    if (!(ws(nx) || nx == '>' || nx == '%' || nx == '[')) {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c->line;
      return -1;
    }
  }
  dv(p, c, 1);
  return 0;
}

static char *dt_grow(char *b, size_t *cap, size_t need) {
  if (need <= *cap)
    return b;
  size_t c = *cap ? *cap : 128;
  while (c < need)
    c *= 2;
  char *q = realloc(b, c);
  if (q)
    *cap = c;
  return q;
}

static int dt_put(char **b, size_t *len, size_t *cap, const char *s,
                  size_t n) {
  char *q = dt_grow(*b, cap, *len + n + 1);
  if (!q)
    return -1;
  *b = q;
  memcpy(*b + *len, s, n);
  *len += n;
  (*b)[*len] = 0;
  return 0;
}

static int dt_putcp(char **b, size_t *len, size_t *cap, unsigned long v) {
  unsigned char out[4];
  size_t n;
  if (v < 0x80) {
    out[0] = (unsigned char)v;
    n = 1;
  } else if (v < 0x800) {
    out[0] = (unsigned char)(0xC0 | (v >> 6));
    out[1] = (unsigned char)(0x80 | (v & 0x3F));
    n = 2;
  } else if (v < 0x10000) {
    out[0] = (unsigned char)(0xE0 | (v >> 12));
    out[1] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
    out[2] = (unsigned char)(0x80 | (v & 0x3F));
    n = 3;
  } else {
    out[0] = (unsigned char)(0xF0 | (v >> 18));
    out[1] = (unsigned char)(0x80 | ((v >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (v & 0x3F));
    n = 4;
  }
  return dt_put(b, len, cap, (const char *)out, n);
}

static int dtd_ref_value(struct XML_ParserStruct *p, DCur *c, char **b,
                         size_t *bl, size_t *bc) {
  size_t s;
  if (c->i >= p->dtdlen)
    return 1;
  if (p->dtd[c->i] == '#') {
    int hex = 0;
    unsigned long v = 0;
    int digits = 0;
    dv(p, c, 1);
    if (c->i >= p->dtdlen)
      return 1;
    if (p->dtd[c->i] == 'x') {
      hex = 1;
      dv(p, c, 1);
    }
    for (;;) {
      unsigned char ch;
      int d;
      if (c->i >= p->dtdlen)
        return 1;
      ch = p->dtd[c->i];
      if (ch == ';')
        break;
      if (hex) {
        if (ch >= '0' && ch <= '9')
          d = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
          d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
          d = ch - 'A' + 10;
        else
          d = -1;
      } else
        d = (ch >= '0' && ch <= '9') ? ch - '0' : -1;
      if (d < 0) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c->line;
        return -1;
      }
      if (v <= 0x110000)
        v = v * (hex ? 16 : 10) + (unsigned long)d;
      digits++;
      dv(p, c, 1);
    }
    if (!digits) {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c->line;
      return -1;
    }
    dv(p, c, 1);
    if (!xml_char(v)) {
      p->err = XML_ERROR_BAD_CHAR_REF;
      p->line = c->line;
      return -1;
    }
    if (dt_putcp(b, bl, bc, v))
      return -2;
    return 0;
  }
  s = c->i;
  {
    size_t nl, ns;
    int r = dt_name(p, c, &ns, &nl);
    if (r != 0)
      return r;
    if (c->i >= p->dtdlen)
      return 1;
    if (p->dtd[c->i] != ';') {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c->line;
      return -1;
    }
    dv(p, c, 1);
    if ((nl == 3 && memcmp(p->dtd + ns, "amp", 3) == 0)
        || (nl == 2 && memcmp(p->dtd + ns, "lt", 2) == 0)
        || (nl == 2 && memcmp(p->dtd + ns, "gt", 2) == 0)
        || (nl == 4 && memcmp(p->dtd + ns, "apos", 4) == 0)
        || (nl == 4 && memcmp(p->dtd + ns, "quot", 4) == 0)) {
      const char *ref;
      if (nl == 3 && memcmp(p->dtd + ns, "amp", 3) == 0)
        ref = "&#38;";
      else if (nl == 2 && memcmp(p->dtd + ns, "lt", 2) == 0)
        ref = "&#60;";
      else if (nl == 2 && memcmp(p->dtd + ns, "gt", 2) == 0)
        ref = "&#62;";
      else if (nl == 4 && memcmp(p->dtd + ns, "apos", 4) == 0)
        ref = "&#39;";
      else
        ref = "&#34;";
      return dt_put(b, bl, bc, ref, strlen(ref));
    }
    if (dt_put(b, bl, bc, (const char *)p->dtd + s - 1, (c->i - (s - 1))))
      return -2;
    return 0;
  }
}

static int dtd_entity_value(struct XML_ParserStruct *p, DCur *c, char **out,
                            size_t *outlen, XML_Size dl) {
  unsigned char q;
  char *b = NULL;
  size_t bl = 0, bc = 0;
  int pe = 0;
  XML_Size peline = 0;
  if (c->i >= p->dtdlen)
    return 1;
  q = p->dtd[c->i];
  if (q != '"' && q != '\'') {
    p->err = XML_ERROR_SYNTAX;
    p->line = c->line;
    return -1;
  }
  dv(p, c, 1);
  for (;;) {
    unsigned char ch;
    int r;
    if (c->i >= p->dtdlen) {
      free(b);
      return 1;
    }
    ch = p->dtd[c->i];
    if (ch == q) {
      dv(p, c, 1);
      break;
    }
    if (ch == '&') {
      dv(p, c, 1);
      r = dtd_ref_value(p, c, &b, &bl, &bc);
      if (r == 1) {
        free(b);
        return 1;
      }
      if (r < 0) {
        if (pe && r == -1) {
          p->err = XML_ERROR_PARAM_ENTITY_REF;
          p->line = peline;
        }
        free(b);
        return r;
      }
      continue;
    }
    if (ch == '%') {
      size_t j = c->i + 1;
      XML_Size pline = c->line;
      if (j >= p->dtdlen) {
        free(b);
        return 1;
      }
      if (!name_start(p->dtd[j])) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = pline;
        free(b);
        return -1;
      }
      while (j < p->dtdlen) {
        unsigned char d = p->dtd[j];
        if (d < 0x80) {
          if (!name_char(d))
            break;
          j++;
        } else {
          size_t n;
          int ur = dt_u8(p, j, &n);
          if (ur < 0) {
            free(b);
            return 1;
          }
          if (ur == 0) {
            p->err = XML_ERROR_INVALID_TOKEN;
            p->line = pline;
            free(b);
            return -1;
          }
          j += n;
        }
      }
      if (j >= p->dtdlen) {
        if (p->final) {
          p->err = XML_ERROR_UNCLOSED_TOKEN;
          p->line = dl;
          free(b);
          return -1;
        }
        free(b);
        return 1;
      }
      if (p->dtd[j] != ';') {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = pline;
        free(b);
        return -1;
      }
      pe = 1;
      peline = pline;
      dv(p, c, j - c->i);
      continue;
    }
    if (ch == '\r') {
      if (dt_put(&b, &bl, &bc, "\n", 1)) {
        free(b);
        return -2;
      }
      dv(p, c, 1);
      if (c->i < p->dtdlen && p->dtd[c->i] == '\n')
        dv(p, c, 1);
      continue;
    }
    if (ch == '\n') {
      if (dt_put(&b, &bl, &bc, "\n", 1)) {
        free(b);
        return -2;
      }
      dv(p, c, 1);
      continue;
    }
    if (ch < 0x20 && ch != '\t') {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c->line;
      free(b);
      return -1;
    }
    if (ch >= 0x80) {
      size_t n;
      int ur = dt_u8(p, c->i, &n);
      if (ur < 0) {
        free(b);
        return 1;
      }
      if (ur == 0) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c->line;
        free(b);
        return -1;
      }
      if (dt_put(&b, &bl, &bc, (const char *)p->dtd + c->i, n)) {
        free(b);
        return -2;
      }
      dv(p, c, n);
      continue;
    }
    if (dt_put(&b, &bl, &bc, (const char *)&ch, 1)) {
      free(b);
      return -2;
    }
    dv(p, c, 1);
  }
  if (pe) {
    p->err = XML_ERROR_PARAM_ENTITY_REF;
    p->line = peline;
    free(b);
    return -1;
  }
  if (!b) {
    b = malloc(1);
    if (!b)
      return -2;
    b[0] = 0;
  }
  *out = b;
  *outlen = bl;
  return 0;
}

static Ent *ent_find(struct XML_ParserStruct *p, const char *s, size_t n) {
  for (size_t i = 0; i < p->nent; i++)
    if (strlen(p->ents[i].name) == n
        && memcmp(p->ents[i].name, s, n) == 0)
      return &p->ents[i];
  return NULL;
}

static int ent_store(struct XML_ParserStruct *p, const char *name, size_t nl,
                     char *text, size_t tl, int external, int unparsed,
                     int parameter) {
  if (nl == 3 && memcmp(name, "amp", 3) == 0)
    return 0;
  if (nl == 2 && (memcmp(name, "lt", 2) == 0 || memcmp(name, "gt", 2) == 0))
    return 0;
  if (nl == 4
      && (memcmp(name, "apos", 4) == 0 || memcmp(name, "quot", 4) == 0))
    return 0;
  if (ent_find(p, name, nl))
    return 0;
  if (p->nent == p->entcap) {
    size_t c = p->entcap ? p->entcap * 2 : 8;
    Ent *q = realloc(p->ents, c * sizeof(*q));
    if (!q)
      return -1;
    p->ents = q;
    p->entcap = c;
  }
  {
    char *nm = malloc(nl + 1);
    if (!nm)
      return -1;
    memcpy(nm, name, nl);
    nm[nl] = 0;
    p->ents[p->nent].name = nm;
    p->ents[p->nent].text = text;
    p->ents[p->nent].textlen = tl;
    p->ents[p->nent].external = external;
    p->ents[p->nent].unparsed = unparsed;
    p->ents[p->nent].parameter = parameter;
    p->ents[p->nent].open = 0;
    p->nent++;
  }
  return 0;
}

static int dt_token_valid(struct XML_ParserStruct *p, size_t i) {
  unsigned char c;
  if (i >= p->dtdlen)
    return 0;
  c = p->dtd[i];
  if (c == '<') {
    if (i + 1 >= p->dtdlen)
      return 0;
    c = p->dtd[i + 1];
    if (c == '/' )
      return 0;
    if (name_start(c) || c == '?' || c == '!')
      return 1;
    return 0;
  }
  if (c == ']' || c == '>' || c == '"' || c == '\'' || c == '%' || c == '#'
      || c == ',' || c == '(' || c == ')' || c == '|' || c == '[')
    return 1;
  if (c >= '0' && c <= '9')
    return 1;
  if (c < 0x80)
    return name_start(c);
  {
    size_t n;
    return dt_u8(p, i, &n) > 0;
  }
}

static int dt_bad(struct XML_ParserStruct *p, DCur *c, int adjacent) {
  size_t i = c->i;
  unsigned char q;
  if (i >= p->dtdlen) {
    p->wait = 1;
    return R_WAIT;
  }
  q = p->dtd[i];
  if (adjacent) {
    if (q == '>' || q == ')' || q == '|' || q == ','
        || q == '[' || q == '%' || q == '?' || q == '*' || q == '+') {
      p->err = XML_ERROR_SYNTAX;
      p->line = c->line;
      return R_ERROR;
    }
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c->line;
    return R_ERROR;
  }
  if (q == '"' || q == '\'') {
    size_t j = i + 1;
    XML_Size ln = c->line;
    for (;;) {
      if (j >= p->dtdlen) {
        if (!p->final) {
          p->wait = 1;
          return R_WAIT;
        }
        p->err = XML_ERROR_UNCLOSED_TOKEN;
        p->line = c->line;
        return R_ERROR;
      }
      if (p->dtd[j] == q)
        break;
      if (p->dtd[j] == '\r') {
        ln++;
        if (j + 1 < p->dtdlen && p->dtd[j + 1] == '\n')
          j++;
        j++;
        continue;
      }
      if (p->dtd[j] == '\n') {
        ln++;
        j++;
        continue;
      }
      if (p->dtd[j] < 0x20 && p->dtd[j] != '\t') {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = ln;
        return R_ERROR;
      }
      if (p->dtd[j] >= 0x80) {
        size_t un;
        int ur = dt_u8(p, j, &un);
        if (ur < 0) {
          if (!p->final) {
            p->wait = 1;
            return R_WAIT;
          }
          p->err = XML_ERROR_PARTIAL_CHAR;
          p->line = ln;
          return R_ERROR;
        }
        if (ur == 0) {
          p->err = XML_ERROR_INVALID_TOKEN;
          p->line = ln;
          return R_ERROR;
        }
        j += un;
        continue;
      }
      j++;
    }
    if (j + 1 >= p->dtdlen) {
      if (!p->final) {
        p->wait = 1;
        return R_WAIT;
      }
      p->err = XML_ERROR_SYNTAX;
      p->line = ln;
      return R_ERROR;
    }
    {
      unsigned char nx = p->dtd[j + 1];
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r'
          || nx == '>' || nx == '%' || nx == '[') {
        p->err = XML_ERROR_SYNTAX;
        p->line = ln;
      } else {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = ln;
      }
    }
    return R_ERROR;
  }
  if (q == '#') {
    size_t j;
    if (i + 1 >= p->dtdlen) {
      if (!p->final) {
        p->wait = 1;
        return R_WAIT;
      }
      p->err = XML_ERROR_UNCLOSED_TOKEN;
      p->line = c->line;
      return R_ERROR;
    }
    if (!name_start(p->dtd[i + 1])) {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c->line;
      return R_ERROR;
    }
    for (j = i + 1; j < p->dtdlen && name_char(p->dtd[j]); j++)
      ;
    if (j >= p->dtdlen) {
      if (!p->final) {
        p->wait = 1;
        return R_WAIT;
      }
      p->err = XML_ERROR_SYNTAX;
      p->line = c->line;
      return R_ERROR;
    }
    {
      unsigned char nx = p->dtd[j];
      if (ws(nx) || nx == ')' || nx == '>' || nx == '%' || nx == '|') {
        p->err = XML_ERROR_SYNTAX;
        p->line = c->line;
      } else {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c->line;
      }
    }
    return R_ERROR;
  }
  if (q == '%') {
    size_t j;
    if (i + 1 >= p->dtdlen) {
      if (!p->final) {
        p->wait = 1;
        return R_WAIT;
      }
      p->err = XML_ERROR_UNCLOSED_TOKEN;
      p->line = c->line;
      return R_ERROR;
    }
    if (name_start(p->dtd[i + 1])) {
      for (j = i + 1; j < p->dtdlen && name_char(p->dtd[j]); j++)
        ;
      if (j >= p->dtdlen) {
        if (!p->final) {
          p->wait = 1;
          return R_WAIT;
        }
        p->err = XML_ERROR_UNCLOSED_TOKEN;
        p->line = c->line;
        return R_ERROR;
      }
      if (p->dtd[j] == ';') {
        p->err = XML_ERROR_PARAM_ENTITY_REF;
        p->line = c->line;
      } else {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c->line;
      }
      return R_ERROR;
    }
    if (ws(p->dtd[i + 1]) || p->dtd[i + 1] == '%') {
      p->err = XML_ERROR_SYNTAX;
      p->line = c->line;
      return R_ERROR;
    }
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c->line;
    return R_ERROR;
  }
  if (q == '<') {
    if (i + 1 >= p->dtdlen) {
      if (!p->final) {
        p->wait = 1;
        return R_WAIT;
      }
      p->err = XML_ERROR_UNCLOSED_TOKEN;
      p->line = c->line;
      return R_ERROR;
    }
    {
      unsigned char n1 = p->dtd[i + 1];
      if (n1 == '/') {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c->line;
        return R_ERROR;
      }
      if (n1 == '!' || n1 == '?' || name_start(n1)) {
        p->err = XML_ERROR_SYNTAX;
        p->line = c->line;
        return R_ERROR;
      }
    }
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c->line;
    return R_ERROR;
  }
  if (q == ')') {
    if (i + 1 >= p->dtdlen) {
      if (!p->final) {
        p->wait = 1;
        return R_WAIT;
      }
      p->err = XML_ERROR_SYNTAX;
      p->line = c->line;
      return R_ERROR;
    }
    {
      unsigned char nx = p->dtd[i + 1];
      if (nx == '*' || nx == '?' || nx == '+' || nx == '>' || nx == ','
          || nx == '|' || nx == ')' || ws(nx)) {
        p->err = XML_ERROR_SYNTAX;
        p->line = c->line;
        return R_ERROR;
      }
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c->line;
      return R_ERROR;
    }
  }
  if (q == '>' || q == '(' || q == ']' || q == '|' || q == ','
      || q == '[') {
    p->err = XML_ERROR_SYNTAX;
    p->line = c->line;
    return R_ERROR;
  }
  if (q >= 0x80) {
    size_t n;
    int ur = dt_cp(p, i, &n);
    if (ur < 0) {
      if (!p->final) {
        p->wait = 1;
        return R_WAIT;
      }
      p->err = XML_ERROR_PARTIAL_CHAR;
      p->line = c->line;
      return R_ERROR;
    }
    if (ur == 0) {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c->line;
      return R_ERROR;
    }
    if (cp_name(p, (unsigned long)ur, 1) || cp_name(p, (unsigned long)ur, 0)) {
      p->err = XML_ERROR_SYNTAX;
      p->line = c->line;
      return R_ERROR;
    }
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c->line;
    return R_ERROR;
  }
  if (q == '-' || q == '.' || (q >= '0' && q <= '9') || name_char(q)) {
    size_t j = i;
    while (j < p->dtdlen && name_char(p->dtd[j]))
      j++;
    if (j >= p->dtdlen) {
      if (!p->final) {
        p->wait = 1;
        return R_WAIT;
      }
      p->err = XML_ERROR_SYNTAX;
      p->line = c->line;
      return R_ERROR;
    }
    {
      unsigned char nx = p->dtd[j];
      if (nx == '>' || nx == ')' || nx == ']' || nx == '|' || nx == ','
          || nx == '[' || nx == '%' || ws(nx)) {
        p->err = XML_ERROR_SYNTAX;
        p->line = c->line;
      } else {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c->line;
      }
    }
    return R_ERROR;
  }
  p->err = XML_ERROR_INVALID_TOKEN;
  p->line = c->line;
  return R_ERROR;
}

static int pref_dt(struct XML_ParserStruct *p, size_t i, const char *s);

static int dtd_entity_decl(struct XML_ParserStruct *p) {
  DCur c;
  XML_Size dl = p->dtline;
  size_t ns, nl;
  int r, parameter = 0, external = 0, unparsed = 0;
  char *text = NULL;
  size_t textlen = 0;
  c.i = p->dtdpos;
  c.line = p->dtline;
  c.skip = p->dtskip;
  dv(p, &c, 8);
  if (c.i >= p->dtdlen)
    return dt_need(p, dl, XML_ERROR_UNCLOSED_TOKEN);
  if (!ws(p->dtd[c.i])) {
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c.line;
    return -1;
  }
  dt_ws(p, &c);
  if (c.i >= p->dtdlen)
    return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
  if (p->dtd[c.i] == '%') {
    parameter = 1;
    dv(p, &c, 1);
    if (c.i >= p->dtdlen)
      return dt_need(p, dl, XML_ERROR_UNCLOSED_TOKEN);
    if (!ws(p->dtd[c.i])) {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c.line;
      return -1;
    }
    dt_ws(p, &c);
    if (c.i >= p->dtdlen)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
  }
  {
    size_t nstart = c.i;
    r = dt_name(p, &c, &ns, &nl);
    if (r == 1) {
      if (c.i > nstart)
        return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
      return dt_need(p, dl, XML_ERROR_UNCLOSED_TOKEN);
    }
  }
  if (r < 0)
    return r == -3 ? -1 : dt_bad(p, &c, 0);
  if (c.i >= p->dtdlen)
    return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
  if (!ws(p->dtd[c.i]))
    return dt_bad(p, &c, 1);
  dt_ws(p, &c);
  if (c.i >= p->dtdlen)
    return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
  {
    char *name = malloc(nl + 1);
    if (!name)
      return nomem(p);
    memcpy(name, p->dtd + ns, nl);
    name[nl] = 0;
    if (p->dtd[c.i] == '"' || p->dtd[c.i] == '\'') {
      r = dtd_entity_value(p, &c, &text, &textlen, dl);
      if (r == 1) {
        free(name);
        free(text);
        return dt_need(p, dl, XML_ERROR_UNCLOSED_TOKEN);
      }
      if (r == -2) {
        free(name);
        free(text);
        return nomem(p);
      }
      if (r < 0) {
        free(name);
        free(text);
        return -1;
      }
    } else {
      size_t kw;
      DCur kc = c;
      if (p->final && (pref_dt(p, c.i, "SYSTEM") == 2
                       || pref_dt(p, c.i, "PUBLIC") == 2)) {
        p->err = XML_ERROR_SYNTAX;
        p->line = c.line;
        free(name);
        return -1;
      }
      r = dt_name(p, &kc, &ns, &kw);
      if (r == 1) {
        free(name);
        return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
      }
      if (r < 0) {
        int br;
        if (r == -3) {
          free(name);
          return -1;
        }
        br = dt_bad(p, &c, 0);
        free(name);
        return br;
      }
      if (!((kw == 6 && memcmp(p->dtd + ns, "SYSTEM", 6) == 0)
            || (kw == 6 && memcmp(p->dtd + ns, "PUBLIC", 6) == 0))) {
        p->err = XML_ERROR_SYNTAX;
        p->line = c.line;
        free(name);
        return -1;
      }
      {
        int pub = p->dtd[ns] == 'P';
        size_t os, on;
        dv(p, &c, 6);
        if (c.i >= p->dtdlen) {
          free(name);
          return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
        }
        if (!ws(p->dtd[c.i])) {
          p->err = p->dtd[c.i] == '>' ? XML_ERROR_SYNTAX
                                      : XML_ERROR_INVALID_TOKEN;
          p->line = c.line;
          free(name);
          return -1;
        }
        dt_ws(p, &c);
        if (pub) {
          {
            size_t ls = c.i;
            r = dt_lit2(p, &c, &os, &on, 1);
            if (r == 1) {
              free(name);
              return dt_need(p, c.line,
                             c.i > ls ? XML_ERROR_UNCLOSED_TOKEN
                                      : XML_ERROR_NO_ELEMENTS);
            }
          }
          if (r < 0) {
            free(name);
            return -1;
          }
          if (c.i >= p->dtdlen) {
            free(name);
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          }
          if (!ws(p->dtd[c.i])) {
            p->err = XML_ERROR_INVALID_TOKEN;
            p->line = c.line;
            free(name);
            return -1;
          }
          dt_ws(p, &c);
        }
        {
          size_t ls = c.i;
          r = dt_lit2(p, &c, &os, &on, 0);
          if (r == 1) {
            free(name);
            return dt_need(p, c.line,
                           c.i > ls ? XML_ERROR_UNCLOSED_TOKEN
                                    : XML_ERROR_NO_ELEMENTS);
          }
        }
        if (r < 0) {
          free(name);
          return -1;
        }
        external = 1;
        dt_ws(p, &c);
        if (c.i >= p->dtdlen) {
          free(name);
          return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
        }
        if (p->dtd[c.i] == 'N') {
          size_t nn, nnl;
          DCur nc = c;
          {
            size_t nstart = nc.i;
            r = dt_name(p, &nc, &nn, &nnl);
            if (r == 1) {
              if (p->final && nc.i - nstart < 5) {
                p->err = XML_ERROR_SYNTAX;
                p->line = c.line;
                free(name);
                return -1;
              }
              free(name);
              return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
            }
          }
          if (r < 0 || nnl != 5 || memcmp(p->dtd + nn, "NDATA", 5) != 0) {
            p->err = XML_ERROR_SYNTAX;
            p->line = c.line;
            free(name);
            return -1;
          }
          dv(p, &c, 5);
          if (c.i >= p->dtdlen) {
            free(name);
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          }
          if (!ws(p->dtd[c.i])) {
            p->err = XML_ERROR_SYNTAX;
            p->line = c.line;
            free(name);
            return -1;
          }
          dt_ws(p, &c);
          r = dt_name(p, &c, &nn, &nnl);
          if (r == 1) {
            free(name);
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          }
          if (r < 0) {
            p->err = XML_ERROR_SYNTAX;
            p->line = c.line;
            free(name);
            return -1;
          }
          unparsed = 1;
          dt_ws(p, &c);
        }
      }
    }
    {
      int had = dt_ws(p, &c);
      if (c.i >= p->dtdlen) {
        free(name);
        free(text);
        return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
      }
      if (p->dtd[c.i] != '>') {
        p->err = had ? XML_ERROR_SYNTAX : XML_ERROR_INVALID_TOKEN;
        p->line = c.line;
        free(name);
        free(text);
        return -1;
      }
    }
    dv(p, &c, 1);
    if (ent_store(p, name, nl, text, textlen, external, unparsed, parameter)) {
      free(name);
      free(text);
      return nomem(p);
    }
    free(name);
  }
  p->dtdpos = c.i;
  p->dtline = c.line;
  p->dtskip = c.skip;
  return R_OK;
}

static int dtd_nmtoken(struct XML_ParserStruct *p, DCur *c, size_t *ns,
                       size_t *nl) {
  size_t s = c->i;
  int any = 0;
  if (c->i >= p->dtdlen) {
    if (!p->final)
      return 1;
    p->err = XML_ERROR_UNCLOSED_TOKEN;
    p->line = c->line;
    return -1;
  }
  while (c->i < p->dtdlen) {
    unsigned char ch = p->dtd[c->i];
    if (ch < 0x80) {
      if (!name_char(ch))
        break;
      dv(p, c, 1);
      any = 1;
    } else {
      size_t n;
      int ur = dt_u8(p, c->i, &n);
      if (ur < 0)
        return 1;
      if (ur == 0) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c->line;
        return -1;
      }
      dv(p, c, n);
      any = 1;
    }
  }
  if (!any) {
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c->line;
    return -1;
  }
  if (c->i >= p->dtdlen) {
    if (!p->final)
      return 1;
    *ns = s;
    *nl = c->i - s;
    return 0;
  }
  *ns = s;
  *nl = c->i - s;
  return 0;
}

static int dtd_pound_name(struct XML_ParserStruct *p, DCur *c, size_t *ns,
                          size_t *nl) {
  int r = dtd_nmtoken(p, c, ns, nl);
  if (r != 0)
    return r;
  if (c->i < p->dtdlen) {
    unsigned char t = p->dtd[c->i];
    if (!(ws(t) || t == ')' || t == '>' || t == '%' || t == '|')) {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c->line;
      return -1;
    }
  }
  return 0;
}

static int dtd_enum_list(struct XML_ParserStruct *p, DCur *c, int names) {
  if (c->i >= p->dtdlen || p->dtd[c->i] != '(')
    return 1;
  dv(p, c, 1);
  for (;;) {
    size_t ns, nl;
    int r;
    dt_ws(p, c);
    if (c->i >= p->dtdlen)
      return 1;
    {
      unsigned char ch = p->dtd[c->i];
      if (ch < 0x80) {
        if (names ? !name_start(ch) : !name_char(ch))
          return dt_bad(p, c, 0);
      } else {
        size_t n;
        int ur = dt_u8(p, c->i, &n);
        if (ur < 0)
          return 1;
        if (ur == 0) {
          p->err = XML_ERROR_INVALID_TOKEN;
          p->line = c->line;
          return -1;
        }
      }
    }
    r = dtd_nmtoken(p, c, &ns, &nl);
    if (r != 0)
      return r;
    dt_ws(p, c);
    if (c->i >= p->dtdlen)
      return 1;
    if (p->dtd[c->i] == ')') {
      dv(p, c, 1);
      return 0;
    }
    if (p->dtd[c->i] != '|')
      return dt_bad(p, c, 0);
    dv(p, c, 1);
  }
}

static void adv_dtd_to(struct XML_ParserStruct *p, DCur *c, size_t to);

static int dtd_attvalue(struct XML_ParserStruct *p, DCur *c, int tokenized,
                        XML_Size dl, char **out, size_t *on) {
  unsigned char q;
  size_t vs, ve, wstart;
  XML_Size vline, endl = 0;
  if (c->i >= p->dtdlen)
    return 1;
  q = p->dtd[c->i];
  if (q != '"' && q != '\'') {
    unsigned char ch = q;
    if (ch < 0x80 && !name_start(ch) && ch != '<' && ch != '&')
      p->err = XML_ERROR_SYNTAX;
    else
      p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c->line;
    return -1;
  }
  dv(p, c, 1);
  vs = c->i;
  vline = c->line;
  {
    size_t scan = c->i;
    XML_Size ln = c->line;
    int lskip = c->skip;
    for (;;) {
      unsigned char ch;
      if (scan >= p->dtdlen) {
        if (p->final) {
          p->err = XML_ERROR_UNCLOSED_TOKEN;
          p->line = dl;
          return -1;
        }
        return 1;
      }
      ch = p->dtd[scan];
      if (ch == q)
        break;
      if (ch == '\r') {
        scan++;
        ln++;
        lskip = 1;
        continue;
      }
      if (ch == '\n') {
        scan++;
        if (lskip)
          lskip = 0;
        else
          ln++;
        continue;
      }
      lskip = 0;
      if (ch < 0x20 && ch != '\t') {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = ln;
        return -1;
      }
      if (ch >= 0x80) {
        size_t n;
        int ur = dt_u8(p, scan, &n);
        if (ur < 0) {
          if (p->final) {
            p->err = XML_ERROR_PARTIAL_CHAR;
            p->line = ln;
            return -1;
          }
          return 1;
        }
        if (ur == 0) {
          p->err = XML_ERROR_INVALID_TOKEN;
          p->line = ln;
          return -1;
        }
        scan += n;
        continue;
      }
      scan++;
    }
    ve = scan;
    endl = ln;
  }
  if (ve + 1 >= p->dtdlen && !p->final)
    return 1;
  if (ve + 1 < p->dtdlen) {
    unsigned char nx = p->dtd[ve + 1];
    if (!(ws(nx) || nx == '>' || nx == '%' || nx == '[')) {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = endl;
      return -1;
    }
  }
  adv_dtd_to(p, c, ve + 1);
  wstart = p->worklen;
  if (attr_walk(p, (const char *)p->dtd + vs, ve - vs, vline, 0))
    return -1;
  if (wend(p))
    return nomem(p);
  if (tokenized)
    collapse_str(p->work + wstart);
  {
    size_t len = strlen(p->work + wstart);
    char *d = malloc(len + 1);
    if (!d)
      return nomem(p);
    memcpy(d, p->work + wstart, len + 1);
    *out = d;
    *on = len;
  }
  p->worklen = wstart;
  return 0;
}

static void adv_dtd_to(struct XML_ParserStruct *p, DCur *c, size_t to) {
  while (c->i < to) {
    unsigned char ch = p->dtd[c->i++];
    p->direct++;
    if (ch == '\r') {
      c->line++;
      c->skip = 1;
    } else if (ch == '\n') {
      if (c->skip)
        c->skip = 0;
      else
        c->line++;
    } else
      c->skip = 0;
  }
}

static int dtd_attlist(struct XML_ParserStruct *p) {
  DCur c;
  ElemDef *ed;
  XML_Size dl = p->dtline;
  c.i = p->dtdpos;
  c.line = p->dtline;
  c.skip = p->dtskip;
  dv(p, &c, 9);
  if (c.i >= p->dtdlen)
    return dt_need(p, dl, XML_ERROR_UNCLOSED_TOKEN);
  if (!ws(p->dtd[c.i])) {
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c.line;
    return -1;
  }
  dt_ws(p, &c);
  if (c.i >= p->dtdlen)
    return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
  {
    size_t ns, nl;
    int r = dt_name(p, &c, &ns, &nl);
    if (r == 1)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (r < 0)
      return r == -3 ? -1 : dt_bad(p, &c, 0);
    ed = elem_find(p, (const char *)p->dtd + ns, nl);
    if (!ed) {
      if (p->nedef == p->edefcap) {
        size_t cc = p->edefcap ? p->edefcap * 2 : 8;
        ElemDef *q = realloc(p->edefs, cc * sizeof(*q));
        if (!q)
          return nomem(p);
        p->edefs = q;
        p->edefcap = cc;
      }
      ed = &p->edefs[p->nedef++];
      memset(ed, 0, sizeof(*ed));
      ed->name = malloc(nl + 1);
      if (!ed->name)
        return nomem(p);
      memcpy(ed->name, p->dtd + ns, nl);
      ed->name[nl] = 0;
    }
  }
  for (;;) {
    int had = dt_ws(p, &c);
    size_t ans, anl;
    AttDef *ad;
    int tokenized = 0, has_dflt = 0;
    char *dflt = NULL;
    size_t dlen = 0;
    int r;
    if (c.i >= p->dtdlen)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (p->dtd[c.i] == '>') {
      dv(p, &c, 1);
      break;
    }
    if (!had)
      return dt_bad(p, &c, 1);
    r = dt_name(p, &c, &ans, &anl);
    if (r == 1)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (r < 0)
      return r == -3 ? -1 : dt_bad(p, &c, 0);
    {
      int h2 = dt_ws(p, &c);
      if (c.i >= p->dtdlen)
        return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
      if (!h2)
        return dt_bad(p, &c, 1);
      if (p->dtd[c.i] == '(') {
        r = dtd_enum_list(p, &c, 0);
        if (r != 0)
          return r == 1 ? dt_need(p, c.line, XML_ERROR_NO_ELEMENTS) : -1;
        tokenized = 1;
      } else {
        size_t ts, tn;
        if (!name_start(p->dtd[c.i]))
          return dt_bad(p, &c, 0);
        r = dt_name(p, &c, &ts, &tn);
        if (r == 1)
          return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
        if (r < 0)
          return r == -3 ? -1 : dt_bad(p, &c, 0);
        if (tn == 5 && memcmp(p->dtd + ts, "CDATA", 5) == 0)
          tokenized = 0;
        else if ((tn == 2 && memcmp(p->dtd + ts, "ID", 2) == 0)
                 || (tn == 5 && memcmp(p->dtd + ts, "IDREF", 5) == 0)
                 || (tn == 6 && memcmp(p->dtd + ts, "IDREFS", 6) == 0)
                 || (tn == 6 && memcmp(p->dtd + ts, "ENTITY", 6) == 0)
                 || (tn == 8 && memcmp(p->dtd + ts, "ENTITIES", 8) == 0)
                 || (tn == 7 && memcmp(p->dtd + ts, "NMTOKEN", 7) == 0)
                 || (tn == 8 && memcmp(p->dtd + ts, "NMTOKENS", 8) == 0))
          tokenized = 1;
        else if (tn == 8 && memcmp(p->dtd + ts, "NOTATION", 8) == 0) {
          int h3;
          if (c.i >= p->dtdlen)
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          h3 = dt_ws(p, &c);
          if (c.i >= p->dtdlen)
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          if (!h3 || p->dtd[c.i] != '(')
            return dt_bad(p, &c, !h3);
          r = dtd_enum_list(p, &c, 1);
          if (r != 0)
            return r == 1 ? dt_need(p, c.line, XML_ERROR_NO_ELEMENTS) : -1;
          tokenized = 1;
        } else {
          p->err = XML_ERROR_SYNTAX;
          p->line = c.line;
          return -1;
        }
      }
    }
    {
      int h3 = dt_ws(p, &c);
      if (c.i >= p->dtdlen)
        return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
      if (!h3)
        return dt_bad(p, &c, 1);
      if (p->dtd[c.i] == '#') {
        size_t ps, pl;
        dv(p, &c, 1);
        r = dtd_pound_name(p, &c, &ps, &pl);
        if (r == 1)
          return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
        if (r < 0)
          return -1;
        if (pl == 8 && memcmp(p->dtd + ps, "REQUIRED", 8) == 0) {
        } else if (pl == 7 && memcmp(p->dtd + ps, "IMPLIED", 7) == 0) {
        } else if (pl == 5 && memcmp(p->dtd + ps, "FIXED", 5) == 0) {
          int h4;
          if (c.i >= p->dtdlen)
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          h4 = dt_ws(p, &c);
          if (c.i >= p->dtdlen)
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          if (!h4 || (p->dtd[c.i] != '"' && p->dtd[c.i] != '\''))
            return dt_bad(p, &c, !h4);
          r = dtd_attvalue(p, &c, tokenized, dl, &dflt, &dlen);
          if (r == 1)
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          if (r < 0)
            return -1;
          has_dflt = 1;
        } else {
          p->err = XML_ERROR_SYNTAX;
          p->line = c.line;
          return -1;
        }
      } else if (p->dtd[c.i] == '"' || p->dtd[c.i] == '\'') {
        r = dtd_attvalue(p, &c, tokenized, dl, &dflt, &dlen);
        if (r == 1)
          return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
        if (r < 0)
          return -1;
        has_dflt = 1;
      } else {
        return dt_bad(p, &c, 0);
      }
    }
    ad = attdef_find(ed, (const char *)p->dtd + ans, anl);
    if (!ad) {
      if (ed->n == ed->cap) {
        size_t cc = ed->cap ? ed->cap * 2 : 8;
        AttDef *q = realloc(ed->attrs, cc * sizeof(*q));
        if (!q) {
          free(dflt);
          return nomem(p);
        }
        ed->attrs = q;
        ed->cap = cc;
      }
      ad = &ed->attrs[ed->n++];
      memset(ad, 0, sizeof(*ad));
      ad->name = malloc(anl + 1);
      if (!ad->name) {
        free(dflt);
        return nomem(p);
      }
      memcpy(ad->name, p->dtd + ans, anl);
      ad->name[anl] = 0;
      ad->tokenized = tokenized;
      ad->dflt = dflt;
      ad->dlen = dlen;
      ad->has_dflt = has_dflt;
    } else {
      free(dflt);
    }
  }
  p->dtdpos = c.i;
  p->dtline = c.line;
  p->dtskip = c.skip;
  return R_OK;
}

static int dtd_quant(struct XML_ParserStruct *p, DCur *c) {
  if (c->i < p->dtdlen
      && (p->dtd[c->i] == '?' || p->dtd[c->i] == '*' || p->dtd[c->i] == '+')) {
    dv(p, c, 1);
    if (c->i < p->dtdlen
        && (p->dtd[c->i] == '?' || p->dtd[c->i] == '*'
            || p->dtd[c->i] == '+')) {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c->line;
      return -1;
    }
  }
  return 0;
}

static int dtd_cp(struct XML_ParserStruct *p, DCur *c, int depth) {
  int r;
  if (depth > 4096) {
    p->err = XML_ERROR_SYNTAX;
    p->line = c->line;
    return -1;
  }
  if (c->i >= p->dtdlen)
    return 1;
  if (p->dtd[c->i] == '(') {
    int d2 = depth + 1;
    unsigned char sep;
    if (d2 > 4096) {
      p->err = XML_ERROR_SYNTAX;
      p->line = c->line;
      return -1;
    }
    dv(p, c, 1);
    dt_ws(p, c);
    if (c->i >= p->dtdlen)
      return 1;
    r = dtd_cp(p, c, d2);
    if (r != 0)
      return r;
    dt_ws(p, c);
    if (c->i >= p->dtdlen)
      return 1;
    sep = p->dtd[c->i];
    if (sep == ')') {
      dv(p, c, 1);
      return dtd_quant(p, c);
    }
    if (sep != ',' && sep != '|') {
      int br = dt_bad(p, c, 0);
      return br == R_OK ? 1 : br;
    }
    for (;;) {
      dv(p, c, 1);
      dt_ws(p, c);
      if (c->i >= p->dtdlen)
        return 1;
      r = dtd_cp(p, c, d2);
      if (r != 0)
        return r;
      dt_ws(p, c);
      if (c->i >= p->dtdlen)
        return 1;
      if (p->dtd[c->i] == sep)
        continue;
      if (p->dtd[c->i] == ')') {
        dv(p, c, 1);
        return dtd_quant(p, c);
      }
      {
        int br = dt_bad(p, c, 0);
        return br == R_OK ? 1 : br;
      }
    }
  }
  if (name_start(p->dtd[c->i])) {
    size_t ns, nl;
    r = dt_name(p, c, &ns, &nl);
    if (r != 0)
      return r;
    return dtd_quant(p, c);
  }
  {
    int br = dt_bad(p, c, 0);
    return br == R_OK ? 1 : br;
  }
}

static int dtd_element(struct XML_ParserStruct *p) {
  DCur c;
  XML_Size dl = p->dtline;
  c.i = p->dtdpos;
  c.line = p->dtline;
  c.skip = p->dtskip;
  dv(p, &c, 9);
  if (c.i >= p->dtdlen)
    return dt_need(p, dl, XML_ERROR_UNCLOSED_TOKEN);
  if (!ws(p->dtd[c.i])) {
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c.line;
    return -1;
  }
  dt_ws(p, &c);
  if (c.i >= p->dtdlen)
    return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
  {
    size_t ns, nl;
    int r = dt_name(p, &c, &ns, &nl);
    if (r == 1)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (r < 0)
      return r == -3 ? -1 : dt_bad(p, &c, 0);
  }
  {
    int hw = dt_ws(p, &c);
    if (c.i >= p->dtdlen)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (!hw && p->dtd[c.i] != '>' && p->dtd[c.i] != '|' && p->dtd[c.i] != ','
        && p->dtd[c.i] != ')' && p->dtd[c.i] != ']' && p->dtd[c.i] != '%')
      return dt_bad(p, &c, 1);
  }
  if (p->dtd[c.i] == '(') {
    int r;
    int depth = 1;
    if (c.i + 1 < p->dtdlen && p->dtd[c.i + 1] == '#') {
      size_t hs, hl;
      dv(p, &c, 2);
      r = dtd_pound_name(p, &c, &hs, &hl);
      if (r == 1)
        return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
      if (r < 0)
        return -1;
      if (!(hl == 6 && memcmp(p->dtd + hs, "PCDATA", 6) == 0)) {
        p->err = XML_ERROR_SYNTAX;
        p->line = c.line;
        return -1;
      }
      dt_ws(p, &c);
      if (c.i >= p->dtdlen)
        return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
      if (p->dtd[c.i] == ')') {
        dv(p, &c, 1);
        if (p->dtd[c.i] == '*')
          dv(p, &c, 1);
        else if (c.i < p->dtdlen
                 && (p->dtd[c.i] == '?' || p->dtd[c.i] == '+')) {
          p->err = XML_ERROR_SYNTAX;
          p->line = c.line;
          return -1;
        } else if (c.i < p->dtdlen && ws(p->dtd[c.i])) {
          dt_ws(p, &c);
          if (c.i < p->dtdlen && p->dtd[c.i] == '*') {
            p->err = XML_ERROR_INVALID_TOKEN;
            p->line = c.line;
            return -1;
          }
        }
      } else {
        for (;;) {
          size_t ns, nl;
          if (c.i >= p->dtdlen)
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          if (p->dtd[c.i] != '|')
            return dt_bad(p, &c, 0);
          dv(p, &c, 1);
          dt_ws(p, &c);
          if (c.i >= p->dtdlen)
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          if (!name_start(p->dtd[c.i]))
            return dt_bad(p, &c, 0);
          r = dt_name(p, &c, &ns, &nl);
          if (r == 1)
            return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
          if (r < 0)
            return -1;
          {
            int hw = dt_ws(p, &c);
            if (c.i >= p->dtdlen)
              return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
            if (p->dtd[c.i] == ')')
              break;
            if (p->dtd[c.i] != '|')
              return dt_bad(p, &c, !hw);
          }
        }
        dv(p, &c, 1);
        if (c.i >= p->dtdlen)
          return dt_need(p, c.line, XML_ERROR_SYNTAX);
        if (p->dtd[c.i] != '*') {
          if (ws(p->dtd[c.i])) {
            p->err = XML_ERROR_SYNTAX;
            p->line = c.line;
            return -1;
          }
          return dt_bad(p, &c, 0);
        }
        dv(p, &c, 1);
      }
    } else {
      r = dtd_cp(p, &c, depth);
      if (r == 1)
        return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
      if (r < 0)
        return -1;
    }
  } else if (name_start(p->dtd[c.i])) {
    size_t ks, kl;
    int r = dt_name(p, &c, &ks, &kl);
    if (r == 1)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (r < 0)
      return -1;
    if (!((kl == 5 && memcmp(p->dtd + ks, "EMPTY", 5) == 0)
          || (kl == 3 && memcmp(p->dtd + ks, "ANY", 3) == 0))) {
      p->err = XML_ERROR_SYNTAX;
      p->line = c.line;
      return -1;
    }
  } else {
    return dt_bad(p, &c, 0);
  }
  {
    int had = dt_ws(p, &c);
    if (c.i >= p->dtdlen)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (p->dtd[c.i] != '>')
      return dt_bad(p, &c, !had && c.i && p->dtd[c.i - 1] == ')');
    (void)had;
    dv(p, &c, 1);
  }
  p->dtdpos = c.i;
  p->dtline = c.line;
  p->dtskip = c.skip;
  return R_OK;
}

static int dtd_notation(struct XML_ParserStruct *p) {
  DCur c;
  XML_Size dl = p->dtline;
  c.i = p->dtdpos;
  c.line = p->dtline;
  c.skip = p->dtskip;
  dv(p, &c, 10);
  if (c.i >= p->dtdlen)
    return dt_need(p, dl, XML_ERROR_UNCLOSED_TOKEN);
  if (!ws(p->dtd[c.i])) {
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c.line;
    return -1;
  }
  dt_ws(p, &c);
  if (c.i >= p->dtdlen)
    return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
  {
    size_t ns, nl;
    int r = dt_name(p, &c, &ns, &nl);
    if (r == 1)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (r < 0)
      return r == -3 ? -1 : dt_bad(p, &c, 0);
  }
  {
    int had = dt_ws(p, &c);
    size_t ks, kl;
    int r;
    int pub = 0;
    if (c.i >= p->dtdlen)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (!had)
      return dt_bad(p, &c, 1);
    if (!name_start(p->dtd[c.i]))
      return dt_bad(p, &c, 0);
    r = dt_name(p, &c, &ks, &kl);
    if (r == 1)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (r < 0)
      return -1;
    if (kl == 6 && memcmp(p->dtd + ks, "SYSTEM", 6) == 0)
      pub = 0;
    else if (kl == 6 && memcmp(p->dtd + ks, "PUBLIC", 6) == 0)
      pub = 1;
    else {
      p->err = XML_ERROR_SYNTAX;
      p->line = c.line;
      return -1;
    }
    if (c.i >= p->dtdlen)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    if (!ws(p->dtd[c.i]))
      return dt_bad(p, &c, 1);
    dt_ws(p, &c);
    if (c.i >= p->dtdlen)
      return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
    {
      size_t os, on;
      r = dt_lit2(p, &c, &os, &on, pub);
      if (r == 1) {
        if (p->final) {
          p->err = XML_ERROR_UNCLOSED_TOKEN;
          p->line = dl;
          return -1;
        }
        return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
      }
      if (r < 0)
        return -1;
    }
    if (pub) {
      int had2 = dt_ws(p, &c);
      if (c.i < p->dtdlen
          && (p->dtd[c.i] == '"' || p->dtd[c.i] == '\'')) {
        size_t os, on;
        if (!had2) {
          p->err = XML_ERROR_INVALID_TOKEN;
          p->line = c.line;
          return -1;
        }
        r = dt_lit2(p, &c, &os, &on, 0);
        if (r == 1) {
          if (p->final) {
            p->err = XML_ERROR_UNCLOSED_TOKEN;
            p->line = dl;
            return -1;
          }
          return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
        }
        if (r < 0)
          return -1;
      }
    }
  }
  dt_ws(p, &c);
  if (c.i >= p->dtdlen)
    return dt_need(p, c.line, XML_ERROR_NO_ELEMENTS);
  if (p->dtd[c.i] != '>')
    return dt_bad(p, &c, 0);
  dv(p, &c, 1);
  p->dtdpos = c.i;
  p->dtline = c.line;
  p->dtskip = c.skip;
  return R_OK;
}

static int dtd_comment(struct XML_ParserStruct *p) {
  DCur c;
  char *b = NULL;
  size_t bl = 0, bc = 0;
  XML_Size startline = p->dtline;
  c.i = p->dtdpos;
  c.line = p->dtline;
  c.skip = p->dtskip;
  dv(p, &c, 4);
  for (;;) {
    unsigned char ch;
    if (c.i >= p->dtdlen) {
      free(b);
      return dt_need(p, startline, XML_ERROR_UNCLOSED_TOKEN);
    }
    ch = p->dtd[c.i];
    if (ch == '-') {
      if (c.i + 1 < p->dtdlen && p->dtd[c.i + 1] == '-') {
        if (c.i + 2 >= p->dtdlen) {
          free(b);
          return dt_need(p, startline, XML_ERROR_UNCLOSED_TOKEN);
        }
        if (p->dtd[c.i + 2] != '>') {
          p->err = XML_ERROR_INVALID_TOKEN;
          p->line = c.line;
          free(b);
          return -1;
        }
        dv(p, &c, 3);
        break;
      }
      if (dt_put(&b, &bl, &bc, "-", 1)) {
        free(b);
        return nomem(p);
      }
      dv(p, &c, 1);
      continue;
    }
    if (ch == '\r' || ch == '\n') {
      if (dt_put(&b, &bl, &bc, "\n", 1)) {
        free(b);
        return nomem(p);
      }
      dv(p, &c, 1);
      if (ch == '\r' && c.i < p->dtdlen && p->dtd[c.i] == '\n')
        dv(p, &c, 1);
      continue;
    }
    if (ch < 0x20 && ch != '\t') {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c.line;
      free(b);
      return -1;
    }
    if (ch >= 0x80) {
      size_t n;
      int ur = dt_u8(p, c.i, &n);
      if (ur < 0) {
        free(b);
        return dt_need(p, startline, XML_ERROR_UNCLOSED_TOKEN);
      }
      if (ur == 0) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c.line;
        free(b);
        return -1;
      }
      if (dt_put(&b, &bl, &bc, (const char *)p->dtd + c.i, n)) {
        free(b);
        return nomem(p);
      }
      dv(p, &c, n);
      continue;
    }
    if (dt_put(&b, &bl, &bc, (const char *)&ch, 1)) {
      free(b);
      return nomem(p);
    }
    dv(p, &c, 1);
  }
  if (!b) {
    b = malloc(1);
    if (!b)
      return nomem(p);
    b[0] = 0;
  }
  p->dtdpos = c.i;
  p->dtline = c.line;
  p->dtskip = c.skip;
  if (p->comment) {
    XML_Size saved = p->line;
    p->line = startline;
    p->comment(p->ud, b);
    p->line = saved;
  }
  free(b);
  return R_OK;
}

static int dtd_pi(struct XML_ParserStruct *p) {
  DCur c;
  size_t ns, nl;
  int r;
  XML_Size startline = p->dtline;
  size_t doff;
  char *b = NULL;
  size_t bl = 0, bc = 0;
  c.i = p->dtdpos;
  c.line = p->dtline;
  c.skip = p->dtskip;
  dv(p, &c, 2);
  r = dt_name(p, &c, &ns, &nl);
  if (r == 1)
    return dt_need(p, startline, XML_ERROR_UNCLOSED_TOKEN);
  if (r < 0) {
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c.line;
    return -1;
  }
  if (nl == 3 && memcmp(p->dtd + ns, "xml", 3) == 0) {
    p->err = XML_ERROR_MISPLACED_XML_PI;
    p->line = startline;
    return -1;
  }
  if (dt_put(&b, &bl, &bc, (const char *)p->dtd + ns, nl)) {
    free(b);
    return nomem(p);
  }
  if (dt_put(&b, &bl, &bc, "", 0)) {
    free(b);
    return nomem(p);
  }
  bl++;
  doff = bl;
  if (c.i < p->dtdlen && p->dtd[c.i] != '?' && !ws(p->dtd[c.i])) {
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c.line;
    free(b);
    return -1;
  }
  dt_ws(p, &c);
  for (;;) {
    unsigned char ch;
    if (c.i >= p->dtdlen) {
      free(b);
      return dt_need(p, startline, XML_ERROR_UNCLOSED_TOKEN);
    }
    ch = p->dtd[c.i];
    if (ch == '?') {
      if (c.i + 1 >= p->dtdlen) {
        free(b);
        return dt_need(p, startline, XML_ERROR_UNCLOSED_TOKEN);
      }
      if (p->dtd[c.i + 1] == '>') {
        dv(p, &c, 2);
        break;
      }
      if (dt_put(&b, &bl, &bc, "?", 1)) {
        free(b);
        return nomem(p);
      }
      dv(p, &c, 1);
      continue;
    }
    if (ch == '\r' || ch == '\n') {
      if (dt_put(&b, &bl, &bc, "\n", 1)) {
        free(b);
        return nomem(p);
      }
      dv(p, &c, 1);
      if (ch == '\r' && c.i < p->dtdlen && p->dtd[c.i] == '\n')
        dv(p, &c, 1);
      continue;
    }
    if (ch < 0x20 && ch != '\t') {
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = c.line;
      free(b);
      return -1;
    }
    if (ch >= 0x80) {
      size_t n;
      int ur = dt_u8(p, c.i, &n);
      if (ur < 0) {
        free(b);
        return dt_need(p, startline, XML_ERROR_UNCLOSED_TOKEN);
      }
      if (ur == 0) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = c.line;
        free(b);
        return -1;
      }
      if (dt_put(&b, &bl, &bc, (const char *)p->dtd + c.i, n)) {
        free(b);
        return nomem(p);
      }
      dv(p, &c, n);
      continue;
    }
    if (dt_put(&b, &bl, &bc, (const char *)&ch, 1)) {
      free(b);
      return nomem(p);
    }
    dv(p, &c, 1);
  }
  if (!b) {
    b = malloc(1);
    if (!b)
      return nomem(p);
    b[0] = 0;
  }
  p->dtdpos = c.i;
  p->dtline = c.line;
  p->dtskip = c.skip;
  if (p->pi) {
    XML_Size saved = p->line;
    p->line = startline;
    p->pi(p->ud, b, b + doff);
    p->line = saved;
  }
  free(b);
  return R_OK;
}

static int dtd_pe_ref(struct XML_ParserStruct *p) {
  DCur c;
  size_t ns, nl;
  int r;
  c.i = p->dtdpos;
  c.line = p->dtline;
  c.skip = p->dtskip;
  dv(p, &c, 1);
  r = dt_name(p, &c, &ns, &nl);
  if (r == 1)
    return dt_need(p, c.line, XML_ERROR_UNCLOSED_TOKEN);
  if (r < 0) {
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c.line;
    return -1;
  }
  if (c.i >= p->dtdlen)
    return dt_need(p, c.line, XML_ERROR_UNCLOSED_TOKEN);
  if (p->dtd[c.i] != ';') {
    p->err = XML_ERROR_INVALID_TOKEN;
    p->line = c.line;
    return -1;
  }
  dv(p, &c, 1);
  p->has_param = 1;
  p->dtdpos = c.i;
  p->dtline = c.line;
  p->dtskip = c.skip;
  return R_OK;
}

static int dt_finish(struct XML_ParserStruct *p, XML_Size close_line) {
  size_t left = p->dtdlen - p->dtdpos;
  if (left) {
    if (p->cap < left) {
      unsigned char *q = realloc(p->buf, left);
      if (!q)
        return nomem(p);
      p->buf = q;
      p->cap = left;
    }
    memcpy(p->buf, p->dtd + p->dtdpos, left);
  }
  p->len = left;
  p->pos = 0;
  p->skip = 0;
  p->dt_active = 0;
  p->dt_subset = 0;
  p->line = close_line;
  if (p->dtend)
    p->dtend(p->ud);
  free(p->dt_name);
  free(p->dt_sys);
  free(p->dt_pub);
  p->dt_name = p->dt_sys = p->dt_pub = NULL;
  p->seen_dt = 1;
  return R_OK;
}

static int pref_dt(struct XML_ParserStruct *p, size_t i, const char *s);

static int dtd_header(struct XML_ParserStruct *p) {
  DCur c;
  free(p->dt_name);
  free(p->dt_sys);
  free(p->dt_pub);
  p->dt_name = NULL;
  p->dt_sys = NULL;
  p->dt_pub = NULL;
  int r;
  size_t ns, nl;
  c.i = p->dtdpos;
  c.line = p->dtline;
  c.skip = p->dtskip;
  if (c.i >= p->dtdlen) {
    if (p->final)
      return fail(p, XML_ERROR_UNCLOSED_TOKEN, c.line);
    p->wait = 1;
    return R_OK;
  }
  if (name_char(p->dtd[c.i])) {
    DCur k = c;
    size_t x, y;
    int rr = dt_name(p, &k, &x, &y);
    if (rr == 1) {
      if (p->final)
        return fail(p, XML_ERROR_UNCLOSED_TOKEN, c.line);
      p->wait = 1;
      return R_OK;
    }
    if (k.i >= p->dtdlen) {
      if (p->final)
        return fail(p, XML_ERROR_UNCLOSED_TOKEN, c.line);
      p->wait = 1;
      return R_OK;
    }
    if (ws(p->dtd[k.i]))
      return fail(p, XML_ERROR_SYNTAX, c.line);
    return fail(p, XML_ERROR_INVALID_TOKEN, c.line);
  }
  if (!ws(p->dtd[c.i]))
    return fail(p, XML_ERROR_INVALID_TOKEN, c.line);
  dt_ws(p, &c);
  if (c.i >= p->dtdlen) {
    if (p->final)
      return fail(p, XML_ERROR_NO_ELEMENTS, c.line);
    p->wait = 1;
    return R_OK;
  }
  if (!name_start(p->dtd[c.i])) {
    int br = dt_bad(p, &c, 0);
    if (br >= 0)
      return R_OK;
    return -1;
  }
  r = dt_name(p, &c, &ns, &nl);
  if (r == 1) {
    if (p->final)
      return fail(p, XML_ERROR_NO_ELEMENTS, c.line);
    p->wait = 1;
    return R_OK;
  }
  if (r < 0)
    return fail(p, XML_ERROR_INVALID_TOKEN, c.line);
  p->dt_name = malloc(nl + 1);
  if (!p->dt_name)
    return nomem(p);
  memcpy(p->dt_name, p->dtd + ns, nl);
  p->dt_name[nl] = 0;
  {
    int hname = dt_ws(p, &c);
  if (c.i >= p->dtdlen) {
    if (p->final)
      return fail(p, XML_ERROR_NO_ELEMENTS, c.line);
    p->wait = 1;
    return R_OK;
  }
tail:
  if (p->dtd[c.i] == '>') {
    XML_Size cl = c.line;
    p->dt_have_header = 1;
    {
      XML_Size saved = p->line;
      p->line = cl;
      if (p->dtstart)
        p->dtstart(p->ud, p->dt_name, p->dt_sys, p->dt_pub, 0);
      p->line = saved;
    }
    dv(p, &c, 1);
    p->dtdpos = c.i;
    p->dtline = c.line;
    p->dtskip = c.skip;
    return dt_finish(p, cl);
  }
  if (p->dtd[c.i] == '[') {
    XML_Size bline = c.line;
    p->dt_have_header = 1;
    p->dt_subset = 1;
    p->dtdpos = c.i;
    p->dtline = c.line;
    p->dtskip = c.skip;
    {
      XML_Size saved = p->line;
      p->line = bline;
      if (p->dtstart)
        p->dtstart(p->ud, p->dt_name, p->dt_sys, p->dt_pub, 1);
      p->line = saved;
    }
    dv(p, &c, 1);
    p->dtdpos = c.i;
    p->dtline = c.line;
    p->dtskip = c.skip;
    return R_OK;
  }
  if (!name_start(p->dtd[c.i]))
    return dt_bad(p, &c, !hname);
  }
  {
    int pub = 0;
    size_t os, on;
    if (p->final && (pref_dt(p, c.i, "SYSTEM") == 2
                     || pref_dt(p, c.i, "PUBLIC") == 2))
      return fail(p, XML_ERROR_SYNTAX, c.line);
    r = dt_name(p, &c, &ns, &nl);
    if (r == 1) {
      if (p->final)
        return fail(p, XML_ERROR_NO_ELEMENTS, c.line);
      p->wait = 1;
      return R_OK;
    }
    if (r < 0)
      return fail(p, XML_ERROR_INVALID_TOKEN, c.line);
    if (c.i >= p->dtdlen) {
      if (p->final)
        return fail(p, XML_ERROR_NO_ELEMENTS, c.line);
      p->wait = 1;
      return R_OK;
    }
    if (!ws(p->dtd[c.i])) {
      if (p->dtd[c.i] == '>')
        return fail(p, XML_ERROR_SYNTAX, c.line);
      return fail(p, XML_ERROR_INVALID_TOKEN, c.line);
    }
    if (nl == 6 && memcmp(p->dtd + ns, "SYSTEM", 6) == 0)
      pub = 0;
    else if (nl == 6 && memcmp(p->dtd + ns, "PUBLIC", 6) == 0)
      pub = 1;
    else
      return fail(p, XML_ERROR_SYNTAX, c.line);
    dt_ws(p, &c);
    if (c.i >= p->dtdlen) {
      if (p->final)
        return fail(p, XML_ERROR_NO_ELEMENTS, c.line);
      p->wait = 1;
      return R_OK;
    }
    if (pub) {
      {
        size_t ls = c.i;
        r = dt_lit2(p, &c, &os, &on, 1);
        if (r == 1) {
          if (p->final)
            return fail(p, c.i > ls ? XML_ERROR_UNCLOSED_TOKEN
                                    : XML_ERROR_NO_ELEMENTS,
                        c.line);
          p->wait = 1;
          return R_OK;
        }
      }
      if (r < 0)
        return -1;
      p->dt_pub = malloc(on + 1);
      if (!p->dt_pub)
        return nomem(p);
      memcpy(p->dt_pub, p->dtd + os, on);
      p->dt_pub[on] = 0;
      if (!dt_ws(p, &c)) {
        if (p->final && c.i >= p->dtdlen)
          return fail(p, XML_ERROR_NO_ELEMENTS, c.line);
        if (c.i >= p->dtdlen) {
          p->wait = 1;
          return R_OK;
        }
        return fail(p, XML_ERROR_INVALID_TOKEN, c.line);
      }
      if (c.i >= p->dtdlen) {
        if (p->final)
          return fail(p, XML_ERROR_NO_ELEMENTS, c.line);
        p->wait = 1;
        return R_OK;
      }
    }
    {
      size_t ls = c.i;
      r = dt_lit2(p, &c, &os, &on, 0);
      if (r == 1) {
        if (p->final)
          return fail(p, c.i > ls ? XML_ERROR_UNCLOSED_TOKEN
                                  : XML_ERROR_NO_ELEMENTS,
                      c.line);
        p->wait = 1;
        return R_OK;
      }
    }
    if (r < 0)
      return -1;
    p->dt_sys = malloc(on + 1);
    if (!p->dt_sys)
      return nomem(p);
    memcpy(p->dt_sys, p->dtd + os, on);
    p->dt_sys[on] = 0;
    p->has_param = 1;
  }
  {
    int had = dt_ws(p, &c);
    if (c.i >= p->dtdlen) {
      if (p->final)
        return fail(p, XML_ERROR_NO_ELEMENTS, c.line);
      p->wait = 1;
      return R_OK;
    }
    if (p->dtd[c.i] != '>' && p->dtd[c.i] != '[') {
      if (c.i + 1 >= p->dtdlen && p->dtd[c.i] == '<') {
        if (p->final)
          return fail(p, XML_ERROR_UNCLOSED_TOKEN, c.line);
        p->wait = 1;
        return R_OK;
      }
      if (!had || !dt_token_valid(p, c.i))
        return fail(p, XML_ERROR_INVALID_TOKEN, c.line);
      return fail(p, XML_ERROR_SYNTAX, c.line);
    }
  }
  p->dtdpos = c.i;
  p->dtline = c.line;
  p->dtskip = c.skip;
  goto tail;
}

static int pref_dt(struct XML_ParserStruct *p, size_t i, const char *s) {
  size_t n = strlen(s), rem = p->dtdlen - i;
  if (rem >= n) {
    if (memcmp(p->dtd + i, s, n) != 0)
      return 0;
    if (n > 2 && s[0] == '<' && s[1] == '!' && name_start((unsigned char)s[2]) && i + n < p->dtdlen
        && name_char(p->dtd[i + n]))
      return 0;
    return 1;
  }
  return memcmp(p->dtd + i, s, rem) == 0 ? 2 : 0;
}

static int dt_decl(struct XML_ParserStruct *p,
                   int (*fn)(struct XML_ParserStruct *)) {
  unsigned long long d = p->direct, i = p->indirect;
  int r = fn(p);
  if (r >= 0 && p->wait) {
    p->direct = d;
    p->indirect = i;
  }
  return r;
}

static int dtd_run(struct XML_ParserStruct *p) {
  for (;;) {
    if (!p->dt_active)
      return R_OK;
    if (p->wait)
      return R_OK;
    if (dt_append(p))
      return nomem(p);
    if (!p->dt_have_header) {
      int r = dt_decl(p, dtd_header);
      if (r < 0 || p->wait || !p->dt_active)
        return r;
      continue;
    }
    if (!p->dt_subset) {
      if (p->dtdpos >= p->dtdlen) {
        if (p->final)
          return fail(p, XML_ERROR_NO_ELEMENTS, p->dtline);
        p->wait = 1;
        return R_OK;
      }
      continue;
    }
    if (p->dtdpos >= p->dtdlen) {
      if (p->final)
        return fail(p, XML_ERROR_NO_ELEMENTS, p->dtline);
      p->wait = 1;
      return R_OK;
    }
    {
      DCur c;
      c.i = p->dtdpos;
      c.line = p->dtline;
      c.skip = p->dtskip;
      dt_ws(p, &c);
      p->dtdpos = c.i;
      p->dtline = c.line;
      p->dtskip = c.skip;
    }
    if (p->dtdpos >= p->dtdlen) {
      if (p->final)
        return fail(p, XML_ERROR_NO_ELEMENTS, p->dtline);
      p->wait = 1;
      return R_OK;
    }
    if (p->dtd[p->dtdpos] == ']') {
      DCur c;
      XML_Size cl;
      c.i = p->dtdpos;
      c.line = p->dtline;
      c.skip = p->dtskip;
      dv(p, &c, 1);
      dt_ws(p, &c);
      if (c.i >= p->dtdlen) {
        if (p->final)
          return fail(p, XML_ERROR_NO_ELEMENTS, c.line);
        p->wait = 1;
        return R_OK;
      }
      if (p->dtd[c.i] != '>')
        return dt_bad(p, &c, 0);
      cl = c.line;
      dv(p, &c, 1);
      p->dtdpos = c.i;
      p->dtline = c.line;
      p->dtskip = c.skip;
      return dt_finish(p, cl);
    }
    if (p->dtd[p->dtdpos] == '<') {
      if (p->dtdpos + 1 >= p->dtdlen) {
        if (p->final)
          return fail(p, XML_ERROR_UNCLOSED_TOKEN, p->dtline);
        p->wait = 1;
        return R_OK;
      }
      if (p->dtd[p->dtdpos + 1] >= 0xC0 && p->dtd[p->dtdpos + 1] <= 0xF4) {
        p->err = XML_ERROR_SYNTAX;
        p->line = p->dtline;
        return -1;
      }
      if (p->dtd[p->dtdpos + 1] >= 0x80) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = p->dtline;
        return -1;
      }
      if (pref_dt(p, p->dtdpos, "<!--") == 1)
        return dt_decl(p, dtd_comment);
      if (pref_dt(p, p->dtdpos, "<?") == 1)
        return dt_decl(p, dtd_pi);
      if (pref_dt(p, p->dtdpos, "<!ENTITY") == 1)
        return dt_decl(p, dtd_entity_decl);
      if (pref_dt(p, p->dtdpos, "<!ELEMENT") == 1)
        return dt_decl(p, dtd_element);
      if (pref_dt(p, p->dtdpos, "<!NOTATION") == 1)
        return dt_decl(p, dtd_notation);
      if (pref_dt(p, p->dtdpos, "<!ATTLIST") == 1)
        return dt_decl(p, dtd_attlist);
      if (pref_dt(p, p->dtdpos, "<!--") == 2
          || pref_dt(p, p->dtdpos, "<?") == 2
          || pref_dt(p, p->dtdpos, "<!ENTITY") == 2
          || pref_dt(p, p->dtdpos, "<!ELEMENT") == 2
          || pref_dt(p, p->dtdpos, "<!ATTLIST") == 2
          || pref_dt(p, p->dtdpos, "<!NOTATION") == 2) {
        if (p->final)
          return fail(p, XML_ERROR_UNCLOSED_TOKEN, p->dtline);
        p->wait = 1;
        return R_OK;
      }
      if (pref_dt(p, p->dtdpos, "<![") == 1) {
        p->err = XML_ERROR_SYNTAX;
        p->line = p->dtline;
        return -1;
      }
      if (name_start(p->dtd[p->dtdpos + 1])) {
        p->err = XML_ERROR_SYNTAX;
        p->line = p->dtline;
        return -1;
      }
      if (p->dtd[p->dtdpos + 1] == '!' && p->dtdpos + 2 < p->dtdlen
          && p->dtd[p->dtdpos + 2] < 0x80
          && name_start(p->dtd[p->dtdpos + 2])) {
        size_t j = p->dtdpos + 2;
        while (j < p->dtdlen && p->dtd[j] < 0x80
               && name_start(p->dtd[j]))
          j++;
        if (j >= p->dtdlen) {
          if (p->final)
            return fail(p, XML_ERROR_UNCLOSED_TOKEN, p->dtline);
          p->wait = 1;
          return R_OK;
        }
        if (ws(p->dtd[j])) {
          p->err = XML_ERROR_SYNTAX;
          p->line = p->dtline;
          return -1;
        }
        if (p->dtd[j] == '%') {
          if (j + 1 >= p->dtdlen) {
            if (p->final)
              return fail(p, XML_ERROR_UNCLOSED_TOKEN, p->dtline);
            p->wait = 1;
            return R_OK;
          }
          if (ws(p->dtd[j + 1]) || p->dtd[j + 1] == '%') {
            p->err = XML_ERROR_INVALID_TOKEN;
            p->line = p->dtline;
            return -1;
          }
          p->err = XML_ERROR_SYNTAX;
          p->line = p->dtline;
          return -1;
        }
      }
      p->err = XML_ERROR_INVALID_TOKEN;
      p->line = p->dtline;
      return -1;
    }
    if (name_start(p->dtd[p->dtdpos])) {
      DCur c;
      size_t x, y;
      int rr;
      c.i = p->dtdpos;
      c.line = p->dtline;
      c.skip = p->dtskip;
      rr = dt_name(p, &c, &x, &y);
      if (rr == 1) {
        if (p->final)
          return fail(p, XML_ERROR_UNCLOSED_TOKEN, p->dtline);
        p->wait = 1;
        return R_OK;
      }
      if (rr < 0) {
        p->err = XML_ERROR_INVALID_TOKEN;
        p->line = p->dtline;
        return -1;
      }
      if (c.i < p->dtdlen
          && (ws(p->dtd[c.i]) || p->dtd[c.i] == '>')) {
        p->err = XML_ERROR_SYNTAX;
        p->line = p->dtline;
        return -1;
      }
    }
    if (p->dtd[p->dtdpos] == '%')
      return dt_decl(p, dtd_pe_ref);
    {
      DCur c;
      c.i = p->dtdpos;
      c.line = p->dtline;
      c.skip = p->dtskip;
      return dt_bad(p, &c, 0);
    }
  }
}

static int doctype(struct XML_ParserStruct *p, XML_Size tline) {
  p->dtdlen = 0;
  p->dtdpos = 0;
  p->dtline = tline;
  p->dtskip = 0;
  p->dt_active = 1;
  p->dt_subset = 0;
  p->dt_have_header = 0;
  if (dt_append(p))
    return nomem(p);
  if (p->dtdlen < 9)
    return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
  p->dtdpos = 9;
  return dtd_run(p);
}

static int content(struct XML_ParserStruct *p) {
  for (;;) {
    if (p->wait)
      return R_OK;
    if (amp_check(p))
      return -1;
    if (p->pos == p->len) {
      if (p->efn) {
        if (pop_ent(p))
          return -1;
        continue;
      }
      return R_OK;
    }
    {
      unsigned char c = p->buf[p->pos];
      if (c == '&') {
        int r = ref_text(p);
        if (r < 0)
          return -1;
        continue;
      }
      if (c == '<') {
        XML_Size tline = p->line;
        size_t rem = p->len - p->pos;
        if (rem < 2) {
          if (p->final)
            return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
          p->wait = 1;
          return R_OK;
        }
        c = p->buf[p->pos + 1];
        if (c == '/') {
          int r = end_tag(p, tline);
          if (r < 0)
            return -1;
          if (p->state != STATE_CONTENT)
            return R_OK;
          continue;
        }
        if (c == '?') {
          int r = pi(p, tline, 0);
          if (r < 0)
            return -1;
          continue;
        }
        if (c == '!') {
          int a = pref(p, "<!--");
          int b = pref(p, "<![CDATA[");
          if (a == 1) {
            int r = comment(p, tline);
            if (r < 0)
              return -1;
            continue;
          }
          if (b == 1) {
            int r = cdata(p, tline);
            if (r < 0)
              return -1;
            continue;
          }
          if (a == 2 || b == 2) {
            if (p->final) {
              if (rem >= 9 && memcmp(p->buf + p->pos, "<![CDATA[", 9) == 0)
                return fail(p, XML_ERROR_UNCLOSED_CDATA_SECTION, p->line);
              return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
            }
            p->wait = 1;
            return R_OK;
          }
          return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
        }
        if (name_start(c)) {
          int r = start_tag(p, tline, p->depth == 0);
          if (r < 0)
            return -1;
          if (p->state != STATE_CONTENT)
            return R_OK;
          continue;
        }
        return fail(p, XML_ERROR_INVALID_TOKEN, tline);
      }
      {
        int r = text_run(p);
        if (r < 0)
          return -1;
      }
    }
  }
}

static int prolog(struct XML_ParserStruct *p) {
  for (;;) {
    if (p->wait)
      return R_OK;
    if (p->pos == p->len)
      return R_OK;
    {
      unsigned char c = p->buf[p->pos];
      if (ws(c)) {
        p->at_start = 0;
        skip_ws(p);
        continue;
      }
      if (c != '<') {
        p->at_start = 0;
        if (c == '"' || c == '\'') {
          size_t j = p->pos + 1;
          while (j < p->len && p->buf[j] != c)
            j++;
          if (j == p->len) {
            if (p->final)
              return fail(p, XML_ERROR_UNCLOSED_TOKEN, p->line);
            p->wait = 1;
            return R_OK;
          }
          return fail(p, XML_ERROR_SYNTAX, p->line);
        }
        if (c == ']' || c == '>' || c == '[')
          return fail(p, XML_ERROR_SYNTAX, p->line);
        {
          size_t st = p->pos, j = p->pos;
          XML_Size sl = p->line;
          for (;;) {
            unsigned char d;
            if (j >= p->len) {
              if (p->final)
                return fail(p, XML_ERROR_SYNTAX, sl);
              p->pos = st;
              p->line = sl;
              p->wait = 1;
              return R_OK;
            }
            d = p->buf[j];
            if (d < 0x20 && d != '\t' && d != '\n' && d != '\r')
              return fail(p, XML_ERROR_INVALID_TOKEN, sl);
            if (d >= 0x80) {
              size_t n;
              int r = u8(p, j, &n);
              if (r < 0) {
                if (p->final)
                  return fail(p, XML_ERROR_PARTIAL_CHAR, sl);
                p->pos = st;
                p->line = sl;
                p->wait = 1;
                return R_OK;
              }
              if (r == 0)
                return fail(p, XML_ERROR_INVALID_TOKEN, sl);
              j += n;
              continue;
            }
            if (!name_char(d))
              break;
            j++;
          }
          if (j == st) {
            unsigned char d0 = p->buf[st];
            if (d0 == '|' || d0 == ',' || d0 == '(' || d0 == ')')
              return fail(p, XML_ERROR_SYNTAX, sl);
            return fail(p, XML_ERROR_INVALID_TOKEN, sl);
          }
          if (ws(p->buf[j]) || p->buf[j] == '>' || p->buf[j] == ')'
              || p->buf[j] == ',' || p->buf[j] == '|' || p->buf[j] == '['
              || p->buf[j] == '%')
            return fail(p, XML_ERROR_SYNTAX, sl);
          return fail(p, XML_ERROR_INVALID_TOKEN, sl);
        }
      }
      {
        XML_Size tline = p->line;
        size_t rem = p->len - p->pos;
        if (rem < 2) {
          if (p->final)
            return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
          p->wait = 1;
          return R_OK;
        }
        c = p->buf[p->pos + 1];
        if (c == '?') {
          int r = pi(p, tline, 1);
          if (r < 0)
            return -1;
          continue;
        }
        if (c == '!') {
          int a = pref(p, "<!--");
          int b = pref(p, "<![CDATA[");
          int d = pref(p, "<!DOCTYPE");
          if (a == 1) {
            int r = comment(p, tline);
            if (r < 0)
              return -1;
            continue;
          }
          if (d == 1) {
            int r;
            if (p->seen_dt) {
              if (p->pos + 9 == p->len) {
                if (p->final)
                  return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
                p->wait = 1;
                return R_OK;
              }
              if (p->pos + 9 < p->len && !ws(p->buf[p->pos + 9]))
                return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
              return fail(p, XML_ERROR_SYNTAX, tline);
            }
            r = doctype(p, tline);
            if (r < 0)
              return -1;
            if (p->wait || p->dt_active)
              return R_OK;
            continue;
          }
          if (b == 1) {
            if (p->final)
              return fail(p, XML_ERROR_SYNTAX, tline);
            p->wait = 1;
            return R_OK;
          }
          if (a == 2 || b == 2) {
            if (p->final)
              return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
            p->wait = 1;
            return R_OK;
          }
          if (d == 2) {
            if (p->final)
              return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
            p->wait = 1;
            return R_OK;
          }
          {
            unsigned char d0 = p->len - p->pos > 2 ? p->buf[p->pos + 2] : 0;
            if (d0 && ((d0 >= 'A' && d0 <= 'Z') || (d0 >= 'a' && d0 <= 'z')
                       || d0 == '_')) {
              size_t i = p->pos + 2;
              for (;;) {
                if (i == p->len) {
                  if (p->final)
                    return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
                  p->wait = 1;
                  return R_OK;
                }
                {
                  unsigned char d = p->buf[i];
                  if ((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z')
                      || d == '_' || d == ':') {
                    i++;
                    continue;
                  }
                  if (ws(d) || d == '>')
                    return fail(p, d == '>' ? XML_ERROR_INVALID_TOKEN
                                            : XML_ERROR_SYNTAX,
                                tline);
                  return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
                }
              }
            }
          }
          return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
        }
        if (name_start(c)) {
          int r = start_tag(p, tline, 1);
          if (r < 0)
            return -1;
          if (p->state == STATE_PROLOG)
            p->state = STATE_CONTENT;
          p->at_start = 0;
          return R_OK;
        }
        return fail(p, XML_ERROR_INVALID_TOKEN, tline);
      }
    }
  }
}

static int epilog(struct XML_ParserStruct *p) {
  for (;;) {
    if (p->wait)
      return R_OK;
    if (p->pos == p->len)
      return R_OK;
    {
      unsigned char c = p->buf[p->pos];
      if (ws(c)) {
        skip_ws(p);
        continue;
      }
      if (c == ']')
        return fail(p, XML_ERROR_JUNK_AFTER_DOC_ELEMENT, p->line);
      if (c == '&')
        return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
      if (c >= 0x80) {
        size_t n;
        int ur = u8(p, p->pos, &n);
        if (ur < 0) {
          if (p->final)
            return fail(p, XML_ERROR_PARTIAL_CHAR, p->line);
          p->wait = 1;
          return R_OK;
        }
        if (ur == 0)
          return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
        return fail(p, XML_ERROR_JUNK_AFTER_DOC_ELEMENT, p->line);
      }
      if (c != '<')
        return fail(p, XML_ERROR_JUNK_AFTER_DOC_ELEMENT, p->line);
      {
        XML_Size tline = p->line;
        size_t rem = p->len - p->pos;
        if (rem < 2) {
          if (p->final)
            return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
          p->wait = 1;
          return R_OK;
        }
        c = p->buf[p->pos + 1];
        if (c == '?') {
          int r = pi(p, tline, 0);
          if (r < 0)
            return -1;
          continue;
        }
        if (c == '!') {
          int a = pref(p, "<!--");
          int b = pref(p, "<![CDATA[");
          int d = pref(p, "<!DOCTYPE");
          if (a == 1) {
            int r = comment(p, tline);
            if (r < 0)
              return -1;
            continue;
          }
          if (b == 1 || d == 1)
            return fail(p, XML_ERROR_JUNK_AFTER_DOC_ELEMENT, tline);
          if (a == 2 || b == 2 || d == 2) {
            if (p->final)
              return fail(p, XML_ERROR_UNCLOSED_TOKEN, tline);
            p->wait = 1;
            return R_OK;
          }
          return fail(p, XML_ERROR_INVALID_TOKEN, p->line);
        }
        if (c == '/')
          return fail(p, XML_ERROR_INVALID_TOKEN, tline);
        if (c >= 0x80) {
          size_t n;
          int ur = u8(p, p->pos + 1, &n);
          if (ur < 0) {
            if (p->final)
              return fail(p, XML_ERROR_PARTIAL_CHAR, tline);
            p->wait = 1;
            return R_OK;
          }
          if (ur == 0)
            return fail(p, XML_ERROR_INVALID_TOKEN, tline);
          return fail(p, XML_ERROR_JUNK_AFTER_DOC_ELEMENT, tline);
        }
        if (name_start(c))
          return fail(p, XML_ERROR_JUNK_AFTER_DOC_ELEMENT, tline);
        return fail(p, XML_ERROR_INVALID_TOKEN, tline);
      }
    }
  }
}

static int run(struct XML_ParserStruct *p) {
  for (;;) {
    int r;
    if (p->wait)
      return R_OK;
    if (p->dt_active)
      r = dtd_run(p);
    else if (p->in_cdata)
      r = cdata_body(p, p->cd_tline);
    else if (p->in_comment)
      r = comment_body(p, p->cm_tline);
    else if (p->state == STATE_PROLOG)
      r = prolog(p);
    else if (p->state == STATE_CONTENT)
      r = content(p);
    else
      r = epilog(p);
    if (r < 0)
      return -1;
    if (p->wait)
      return R_OK;
    if (p->dt_active) {
      if (p->dtdpos < p->dtdlen)
        continue;
      if (p->final)
        return fail(p, XML_ERROR_NO_ELEMENTS, p->dtline);
      return R_OK;
    }
    if (p->pos == p->len) {
      if (p->final) {
        if (p->state == STATE_PROLOG || p->state == STATE_CONTENT)
          return fail(p, XML_ERROR_NO_ELEMENTS, p->line);
      }
      return R_OK;
    }
  }
}

static int out_reserve(struct XML_ParserStruct *p, size_t n) {
  if (p->len + n <= p->cap)
    return 0;
  size_t c = p->cap ? p->cap : 1024;
  while (c < p->len + n) {
    if (c > SIZE_MAX / 2)
      return -1;
    c *= 2;
  }
  unsigned char *q = realloc(p->buf, c);
  if (!q)
    return -1;
  p->buf = q;
  p->cap = c;
  return 0;
}

static int out_cp(struct XML_ParserStruct *p, unsigned long v) {
  unsigned char b[4];
  size_t n;
  if (v < 0x80) {
    b[0] = (unsigned char)v;
    n = 1;
  } else if (v < 0x800) {
    b[0] = (unsigned char)(0xC0 | (v >> 6));
    b[1] = (unsigned char)(0x80 | (v & 0x3F));
    n = 2;
  } else if (v < 0x10000) {
    b[0] = (unsigned char)(0xE0 | (v >> 12));
    b[1] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
    b[2] = (unsigned char)(0x80 | (v & 0x3F));
    n = 3;
  } else {
    b[0] = (unsigned char)(0xF0 | (v >> 18));
    b[1] = (unsigned char)(0x80 | ((v >> 12) & 0x3F));
    b[2] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
    b[3] = (unsigned char)(0x80 | (v & 0x3F));
    n = 4;
  }
  if (out_reserve(p, n))
    return -1;
  memcpy(p->buf + p->len, b, n);
  p->len += n;
  return 0;
}

static void decode_input(struct XML_ParserStruct *p) {
  for (;;) {
    size_t avail = p->rawlen - p->rawpos;
    const unsigned char *r;
    if (p->dec_err)
      return;
    if (p->enc == 0) {
      if (avail == 0)
        return;
      if (avail == 1) {
        r = p->raw + p->rawpos;
        if (r[0] == 0xFE || r[0] == 0xFF || r[0] == 0xEF || r[0] == 0x00
            || r[0] == 0x3C) {
          if (p->final) {
            p->dec_err = XML_ERROR_UNCLOSED_TOKEN;
            return;
          }
          return;
        }
        p->enc = 1;
        continue;
      }
      r = p->raw + p->rawpos;
      if (r[0] == 0xEF && r[1] == 0xBB) {
        if (avail < 3) {
          if (p->final)
            p->dec_err = XML_ERROR_UNCLOSED_TOKEN;
          return;
        }
        if (r[2] == 0xBF)
          p->rawpos += 3;
        p->enc = 1;
        continue;
      }
      if (r[0] == 0xFF && r[1] == 0xFE) {
        p->enc = 2;
        p->rawpos += 2;
        continue;
      }
      if (r[0] == 0xFE && r[1] == 0xFF) {
        p->enc = 3;
        p->rawpos += 2;
        continue;
      }
      if (r[0] == 0) {
        p->enc = 3;
        continue;
      }
      if (r[1] == 0) {
        p->enc = 2;
        continue;
      }
      p->enc = 1;
      continue;
    }
    if (p->enc == 1) {
      if (!avail)
        return;
      if (out_reserve(p, avail)) {
        p->dec_err = XML_ERROR_NO_MEMORY;
        return;
      }
      memcpy(p->buf + p->len, p->raw + p->rawpos, avail);
      p->len += avail;
      p->rawpos += avail;
      return;
    }
    if (avail < 2) {
      if (avail == 1 && p->final)
        p->dec_err = XML_ERROR_UNCLOSED_TOKEN;
      return;
    }
    r = p->raw + p->rawpos;
    {
      unsigned unit = p->enc == 2 ? (unsigned)r[0] | ((unsigned)r[1] << 8)
                                 : ((unsigned)r[0] << 8) | (unsigned)r[1];
      if (unit >= 0xD800 && unit <= 0xDBFF) {
        unsigned lo;
        unsigned long cp;
        if (avail < 4) {
          if (p->final)
            p->dec_err = XML_ERROR_PARTIAL_CHAR;
          return;
        }
        lo = p->enc == 2 ? (unsigned)r[2] | ((unsigned)r[3] << 8)
                         : ((unsigned)r[2] << 8) | (unsigned)r[3];
        cp = 0x10000UL + (((unsigned long)unit & 0x3FF) << 10)
             + ((unsigned long)lo & 0x3FF);
        if (out_cp(p, cp)) {
          p->dec_err = XML_ERROR_NO_MEMORY;
          return;
        }
        p->rawpos += 4;
        continue;
      }
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        p->dec_err = XML_ERROR_INVALID_TOKEN;
        return;
      }
      if (out_cp(p, unit)) {
        p->dec_err = XML_ERROR_NO_MEMORY;
        return;
      }
      p->rawpos += 2;
    }
  }
}

XML_Parser XMLCALL XML_ParserCreate(const XML_Char *encodingName) {
  struct XML_ParserStruct *p = calloc(1, sizeof(*p));
  (void)encodingName;
  if (!p)
    return NULL;
  p->line = 1;
  p->at_start = 1;
  p->state = STATE_PROLOG;
  return p;
}

void XMLCALL XML_ParserFree(XML_Parser parser) {
  struct XML_ParserStruct *p = parser;
  if (!p)
    return;
  for (size_t i = 0; i < p->depth; i++)
    free(p->stack[i]);
  free(p->stack);
  free(p->work);
  free(p->av);
  free(p->attp);
  free(p->raw);
  free(p->dtd);
  free(p->dt_name);
  free(p->dt_sys);
  free(p->dt_pub);
  for (size_t i = 0; i < p->nent; i++) {
    free(p->ents[i].name);
    free(p->ents[i].text);
  }
  free(p->ents);
  for (size_t i = 0; i < p->nedef; i++) {
    for (size_t j = 0; j < p->edefs[i].n; j++) {
      free(p->edefs[i].attrs[j].name);
      free(p->edefs[i].attrs[j].dflt);
    }
    free(p->edefs[i].name);
    free(p->edefs[i].attrs);
  }
  free(p->edefs);
  while (p->efn) {
    EntFrame *f = &p->eframes[--p->efn];
    p->buf = f->buf;
    p->len = f->len;
    p->cap = f->cap;
    p->pos = f->pos;
  }
  free(p->eframes);
  free(p->buf);
  free(p);
}

void *XMLCALL XML_GetBuffer(XML_Parser parser, int len) {
  struct XML_ParserStruct *p = parser;
  if (!p)
    return NULL;
  if (len < 0) {
    p->err = XML_ERROR_NO_MEMORY;
    return NULL;
  }
  if (p->done) {
    p->err = p->err ? p->err : XML_ERROR_FINISHED;
    return NULL;
  }
  if (p->pos) {
    memmove(p->buf, p->buf + p->pos, p->len - p->pos);
    p->len -= p->pos;
    p->pos = 0;
  }
  if (p->rawpos) {
    memmove(p->raw, p->raw + p->rawpos, p->rawlen - p->rawpos);
    p->rawlen -= p->rawpos;
    p->rawpos = 0;
  }
  if (!p->raw || p->rawlen + (size_t)len > p->rawcap) {
    size_t c = p->rawcap ? p->rawcap : 1024;
    while (c < p->rawlen + (size_t)len) {
      if (c > SIZE_MAX / 2) {
        p->err = XML_ERROR_NO_MEMORY;
        return NULL;
      }
      c *= 2;
    }
    unsigned char *q = realloc(p->raw, c);
    if (!q) {
      p->err = XML_ERROR_NO_MEMORY;
      return NULL;
    }
    p->raw = q;
    p->rawcap = c;
  }
  p->got = 1;
  return p->raw + p->rawlen;
}

enum XML_Status XMLCALL XML_Parse(XML_Parser parser, const char *s, int len,
                                  int isFinal) {
  void *buf;
  if (!parser)
    return XML_STATUS_ERROR;
  if (len < 0 || (!s && len != 0)) {
    parser->err = XML_ERROR_INVALID_ARGUMENT;
    return XML_STATUS_ERROR;
  }
  buf = XML_GetBuffer(parser, len);
  if (!buf)
    return XML_STATUS_ERROR;
  if (len)
    memcpy(buf, s, (size_t)len);
  return XML_ParseBuffer(parser, len, isFinal);
}

enum XML_Status XMLCALL XML_ParseBuffer(XML_Parser parser, int len,
                                        int isFinal) {
  struct XML_ParserStruct *p = parser;
  if (!p)
    return XML_STATUS_ERROR;
  if (len < 0) {
    p->err = XML_ERROR_INVALID_ARGUMENT;
    return XML_STATUS_ERROR;
  }
  if (p->done) {
    p->err = p->err ? p->err : XML_ERROR_FINISHED;
    return XML_STATUS_ERROR;
  }
  if (!p->got) {
    p->err = XML_ERROR_NO_BUFFER;
    return XML_STATUS_ERROR;
  }
  p->got = 0;
  p->rawlen += (size_t)len;
  p->final = isFinal;
  decode_input(p);
  p->wait = 0;
  if (p->skip && p->pos < p->len && p->buf[p->pos] == '\n')
    p->pos++;
  p->skip = 0;
  if (run(p) < 0) {
    if (p->dec_err && p->err == XML_ERROR_NO_ELEMENTS && p->pos == p->len)
      p->err = p->dec_err;
    p->done = 1;
    return XML_STATUS_ERROR;
  }
  if (p->dec_err && p->pos == p->len) {
    p->err = p->dec_err;
    p->done = 1;
    return XML_STATUS_ERROR;
  }
  if (isFinal) {
    p->done = 1;
    p->final = 1;
  }
  return XML_STATUS_OK;
}

void XMLCALL XML_SetUserData(XML_Parser parser, void *userData) {
  if (parser)
    parser->ud = userData;
}

enum XML_Error XMLCALL XML_GetErrorCode(XML_Parser parser) {
  return parser ? parser->err : XML_ERROR_INVALID_ARGUMENT;
}

XML_Size XMLCALL XML_GetCurrentLineNumber(XML_Parser parser) {
  return parser ? parser->line : 0;
}

const XML_LChar *XMLCALL XML_ErrorString(enum XML_Error code) {
  if (code < 0 || code >= (int)(sizeof(errmsg) / sizeof(errmsg[0])))
    return NULL;
  return errmsg[code];
}

void XMLCALL XML_SetElementHandler(XML_Parser parser,
                                   XML_StartElementHandler start,
                                   XML_EndElementHandler end) {
  if (!parser)
    return;
  parser->start = start;
  parser->end = end;
}

void XMLCALL XML_SetCharacterDataHandler(XML_Parser parser,
                                         XML_CharacterDataHandler handler) {
  if (parser)
    parser->chars = handler;
}

void XMLCALL
XML_SetProcessingInstructionHandler(XML_Parser parser,
                                    XML_ProcessingInstructionHandler handler) {
  if (parser)
    parser->pi = handler;
}

void XMLCALL XML_SetCommentHandler(XML_Parser parser,
                                   XML_CommentHandler handler) {
  if (parser)
    parser->comment = handler;
}

void XMLCALL XML_SetDoctypeDeclHandler(XML_Parser parser,
                                       XML_StartDoctypeDeclHandler start,
                                       XML_EndDoctypeDeclHandler end) {
  if (!parser)
    return;
  parser->dtstart = start;
  parser->dtend = end;
}
