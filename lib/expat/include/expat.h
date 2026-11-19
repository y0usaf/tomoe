#ifndef Expat_INCLUDED
#  define Expat_INCLUDED 1

#  include <stddef.h>

#  ifdef __cplusplus
extern "C" {
#  endif

#  ifndef XMLCALL
#    define XMLCALL
#  endif

#  ifndef XMLPARSEAPI
#    define XMLPARSEAPI(type) type
#  endif

typedef char XML_Char;
typedef char XML_LChar;

struct XML_ParserStruct;
typedef struct XML_ParserStruct *XML_Parser;

typedef unsigned char XML_Bool;
#  define XML_TRUE ((XML_Bool)1)
#  define XML_FALSE ((XML_Bool)0)

enum XML_Status {
  XML_STATUS_ERROR = 0,
#  define XML_STATUS_ERROR XML_STATUS_ERROR
  XML_STATUS_OK = 1,
#  define XML_STATUS_OK XML_STATUS_OK
  XML_STATUS_SUSPENDED = 2
#  define XML_STATUS_SUSPENDED XML_STATUS_SUSPENDED
};

enum XML_Error {
  XML_ERROR_NONE,
  XML_ERROR_NO_MEMORY,
  XML_ERROR_SYNTAX,
  XML_ERROR_NO_ELEMENTS,
  XML_ERROR_INVALID_TOKEN,
  XML_ERROR_UNCLOSED_TOKEN,
  XML_ERROR_PARTIAL_CHAR,
  XML_ERROR_TAG_MISMATCH,
  XML_ERROR_DUPLICATE_ATTRIBUTE,
  XML_ERROR_JUNK_AFTER_DOC_ELEMENT,
  XML_ERROR_PARAM_ENTITY_REF,
  XML_ERROR_UNDEFINED_ENTITY,
  XML_ERROR_RECURSIVE_ENTITY_REF,
  XML_ERROR_ASYNC_ENTITY,
  XML_ERROR_BAD_CHAR_REF,
  XML_ERROR_BINARY_ENTITY_REF,
  XML_ERROR_ATTRIBUTE_EXTERNAL_ENTITY_REF,
  XML_ERROR_MISPLACED_XML_PI,
  XML_ERROR_UNKNOWN_ENCODING,
  XML_ERROR_INCORRECT_ENCODING,
  XML_ERROR_UNCLOSED_CDATA_SECTION,
  XML_ERROR_EXTERNAL_ENTITY_HANDLING,
  XML_ERROR_NOT_STANDALONE,
  XML_ERROR_UNEXPECTED_STATE,
  XML_ERROR_ENTITY_DECLARED_IN_PE,
  XML_ERROR_FEATURE_REQUIRES_XML_DTD,
  XML_ERROR_CANT_CHANGE_FEATURE_ONCE_PARSING,
  XML_ERROR_UNBOUND_PREFIX,
  XML_ERROR_UNDECLARING_PREFIX,
  XML_ERROR_INCOMPLETE_PE,
  XML_ERROR_XML_DECL,
  XML_ERROR_TEXT_DECL,
  XML_ERROR_PUBLICID,
  XML_ERROR_SUSPENDED,
  XML_ERROR_NOT_SUSPENDED,
  XML_ERROR_ABORTED,
  XML_ERROR_FINISHED,
  XML_ERROR_SUSPEND_PE,
  XML_ERROR_RESERVED_PREFIX_XML,
  XML_ERROR_RESERVED_PREFIX_XMLNS,
  XML_ERROR_RESERVED_NAMESPACE_URI,
  XML_ERROR_INVALID_ARGUMENT,
  XML_ERROR_NO_BUFFER,
  XML_ERROR_AMPLIFICATION_LIMIT_BREACH,
  XML_ERROR_NOT_STARTED
};

typedef long XML_Index;
typedef unsigned long XML_Size;

typedef void(XMLCALL *XML_StartElementHandler)(void *userData,
                                               const XML_Char *name,
                                               const XML_Char **atts);
typedef void(XMLCALL *XML_EndElementHandler)(void *userData,
                                             const XML_Char *name);
typedef void(XMLCALL *XML_CharacterDataHandler)(void *userData,
                                                const XML_Char *s, int len);
typedef void(XMLCALL *XML_ProcessingInstructionHandler)(
    void *userData, const XML_Char *target, const XML_Char *data);
typedef void(XMLCALL *XML_CommentHandler)(void *userData, const XML_Char *data);
typedef void(XMLCALL *XML_StartDoctypeDeclHandler)(
    void *userData, const XML_Char *doctypeName, const XML_Char *sysid,
    const XML_Char *pubid, int has_internal_subset);
typedef void(XMLCALL *XML_EndDoctypeDeclHandler)(void *userData);

XMLPARSEAPI(XML_Parser) XMLCALL XML_ParserCreate(const XML_Char *encodingName);
XMLPARSEAPI(void) XMLCALL XML_ParserFree(XML_Parser parser);
XMLPARSEAPI(enum XML_Status) XMLCALL XML_ParseBuffer(XML_Parser parser,
                                                     int len, int isFinal);
XMLPARSEAPI(enum XML_Status) XMLCALL XML_Parse(XML_Parser parser,
                                               const char *s, int len,
                                               int isFinal);
XMLPARSEAPI(void *) XMLCALL XML_GetBuffer(XML_Parser parser, int len);
XMLPARSEAPI(void) XMLCALL XML_SetUserData(XML_Parser parser, void *userData);
XMLPARSEAPI(enum XML_Error) XMLCALL XML_GetErrorCode(XML_Parser parser);
XMLPARSEAPI(XML_Size) XMLCALL XML_GetCurrentLineNumber(XML_Parser parser);
XMLPARSEAPI(const XML_LChar *) XMLCALL XML_ErrorString(enum XML_Error code);
XMLPARSEAPI(void) XMLCALL XML_SetElementHandler(
    XML_Parser parser, XML_StartElementHandler start, XML_EndElementHandler end);
XMLPARSEAPI(void) XMLCALL XML_SetCharacterDataHandler(
    XML_Parser parser, XML_CharacterDataHandler handler);
XMLPARSEAPI(void) XMLCALL XML_SetProcessingInstructionHandler(
    XML_Parser parser, XML_ProcessingInstructionHandler handler);
XMLPARSEAPI(void) XMLCALL
XML_SetCommentHandler(XML_Parser parser, XML_CommentHandler handler);
XMLPARSEAPI(void) XMLCALL XML_SetDoctypeDeclHandler(
    XML_Parser parser, XML_StartDoctypeDeclHandler start,
    XML_EndDoctypeDeclHandler end);

#  ifdef __cplusplus
}
#  endif

#endif
