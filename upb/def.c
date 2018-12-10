
#include "upb/def.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "google/protobuf/descriptor.upb.h"
#include "upb/handlers.h"

typedef struct {
  size_t len;
  char str[1];  /* Null-terminated string data follows. */
} str_t;

static str_t *newstr(const char *data, size_t len) {
  str_t *ret = upb_gmalloc(sizeof(*ret) + len);
  if (!ret) return NULL;
  ret->len = len;
  memcpy(ret->str, data, len);
  ret->str[len] = '\0';
  return ret;
}

static void freestr(str_t *s) { upb_gfree(s); }

struct upb_fielddef {
  const char *full_name;
  union {
    int64_t sint;
    uint64_t uint;
    double dbl;
    float flt;
    bool boolean;
    str_t *str;
  } defaultval;
  const upb_msgdef *msgdef;
  const upb_oneofdef *oneof;
  union {
    const upb_msgdef *msgdef;
    const upb_enumdef *enumdef;
  } sub;
  uint32_t number_;
  uint32_t index_;
  uint32_t selector_base;  /* Used to index into a upb::Handlers table. */
  bool is_extension_;
  bool lazy_;
  bool packed_;
  upb_descriptortype_t type_;
  upb_label_t label_;
};

struct upb_msgdef {
  const upb_filedef *file;
  const char *full_name;
  uint32_t selector_count;
  uint32_t submsg_field_count;

  /* Tables for looking up fields by number and name. */
  upb_inttable itof;
  upb_strtable ntof;

  /* Is this a map-entry message? */
  bool map_entry;
  upb_wellknowntype_t well_known_type;

  /* TODO(haberman): proper extension ranges (there can be multiple). */
};

struct upb_enumdef {
  const char *full_name;
  upb_strtable ntoi;
  upb_inttable iton;
  int32_t defaultval;
};

struct upb_oneofdef {
  const upb_msgdef *parent;
  const char *name;
  uint32_t index;
  upb_strtable ntof;
  upb_inttable itof;
};

struct upb_filedef {
  const char *name;
  const char *package;
  const char *phpprefix;
  const char *phpnamespace;
  upb_syntax_t syntax;

  const upb_msgdef **msgs;
  const upb_enumdef **enums;
  const upb_filedef **deps;

  int msgcount;
  int enumcount;
  int depcount;
};

/* Inside a symtab we store tagged pointers to specific def types. */
typedef enum {
  UPB_DEFTYPE_MSG = 0,
  UPB_DEFTYPE_ENUM = 1,
  UPB_DEFTYPE_FIELD = 2,
  UPB_DEFTYPE_ONEOF = 3
} upb_deftype_t;

static const void *unpack_def(upb_value v, upb_deftype_t type) {
  uintptr_t num = (uintptr_t)upb_value_getconstptr(v);
  return (num & 3) == type ? (const void*)(num & ~3) : NULL;
}

static upb_value pack_def(const void *ptr, upb_deftype_t type) {
  uintptr_t num = (uintptr_t)ptr | type;
  return upb_value_constptr((const void*)num);
}

struct upb_symtab {
  upb_arena arena;
  upb_strtable syms;  /* full_name -> packed def ptr */
  upb_strtable files;  /* file_name -> upb_filedef* */
};

/* isalpha() etc. from <ctype.h> are locale-dependent, which we don't want. */
static bool upb_isbetween(char c, char low, char high) {
  return c >= low && c <= high;
}

static bool upb_isletter(char c) {
  return upb_isbetween(c, 'A', 'Z') || upb_isbetween(c, 'a', 'z') || c == '_';
}

static bool upb_isalphanum(char c) {
  return upb_isletter(c) || upb_isbetween(c, '0', '9');
}

static bool upb_isident(upb_stringview name, bool full, upb_status *s) {
  const char *str = name.data;
  size_t len = name.size;
  bool start = true;
  size_t i;
  for (i = 0; i < len; i++) {
    char c = str[i];
    if (c == '.') {
      if (start || !full) {
        upb_status_seterrf(s, "invalid name: unexpected '.' (%s)", str);
        return false;
      }
      start = true;
    } else if (start) {
      if (!upb_isletter(c)) {
        upb_status_seterrf(
            s, "invalid name: path components must start with a letter (%s)",
            str);
        return false;
      }
      start = false;
    } else {
      if (!upb_isalphanum(c)) {
        upb_status_seterrf(s, "invalid name: non-alphanumeric character (%s)",
                           str);
        return false;
      }
    }
  }
  return !start;
}

static const char *shortdefname(const char *fullname) {
  const char *p;

  if (fullname == NULL) {
    return NULL;
  } else if ((p = strrchr(fullname, '.')) == NULL) {
    /* No '.' in the name, return the full string. */
    return fullname;
  } else {
    /* Return one past the last '.'. */
    return p + 1;
  }
}

static bool upb_isoneof(const void *def) {
  UPB_UNUSED(def);
  return true;
}

static bool upb_isfield(const void *def) {
  UPB_UNUSED(def);
  return true;
}

static const upb_oneofdef *upb_trygetoneof(const void *def) {
  return upb_isoneof(def) ? (const upb_oneofdef*)def : NULL;
}

static const upb_fielddef *upb_trygetfield(const void *def) {
  return upb_isfield(def) ? (const upb_fielddef*)def : NULL;
}


/* All submessage fields are lower than all other fields.
 * Secondly, fields are increasing in order. */
uint32_t field_rank(const upb_fielddef *f) {
  uint32_t ret = upb_fielddef_number(f);
  const uint32_t high_bit = 1 << 30;
  UPB_ASSERT(ret < high_bit);
  if (!upb_fielddef_issubmsg(f))
    ret |= high_bit;
  return ret;
}

int cmp_fields(const void *p1, const void *p2) {
  const upb_fielddef *f1 = *(upb_fielddef*const*)p1;
  const upb_fielddef *f2 = *(upb_fielddef*const*)p2;
  return field_rank(f1) - field_rank(f2);
}

static bool assign_msg_indices(upb_msgdef *m, upb_status *s) {
  /* Sort fields.  upb internally relies on UPB_TYPE_MESSAGE fields having the
   * lowest indexes, but we do not publicly guarantee this. */
  upb_msg_field_iter j;
  upb_msg_oneof_iter k;
  int i;
  uint32_t selector;
  int n = upb_msgdef_numfields(m);
  upb_fielddef **fields;

  if (n == 0) {
    m->selector_count = UPB_STATIC_SELECTOR_COUNT;
    m->submsg_field_count = 0;
    return true;
  }

  fields = upb_gmalloc(n * sizeof(*fields));
  if (!fields) {
    upb_upberr_setoom(s);
    return false;
  }

  m->submsg_field_count = 0;
  for(i = 0, upb_msg_field_begin(&j, m);
      !upb_msg_field_done(&j);
      upb_msg_field_next(&j), i++) {
    upb_fielddef *f = upb_msg_iter_field(&j);
    UPB_ASSERT(f->msgdef == m);
    if (upb_fielddef_issubmsg(f)) {
      m->submsg_field_count++;
    }
    fields[i] = f;
  }

  qsort(fields, n, sizeof(*fields), cmp_fields);

  selector = UPB_STATIC_SELECTOR_COUNT + m->submsg_field_count;
  for (i = 0; i < n; i++) {
    upb_fielddef *f = fields[i];
    f->index_ = i;
    f->selector_base = selector + upb_handlers_selectorbaseoffset(f);
    selector += upb_handlers_selectorcount(f);
  }
  m->selector_count = selector;

#ifndef NDEBUG
  {
    /* Verify that all selectors for the message are distinct. */
#define TRY(type) \
    if (upb_handlers_getselector(f, type, &sel)) upb_inttable_insert(&t, sel, v);

    upb_inttable t;
    upb_value v;
    upb_selector_t sel;

    upb_inttable_init(&t, UPB_CTYPE_BOOL);
    v = upb_value_bool(true);
    upb_inttable_insert(&t, UPB_STARTMSG_SELECTOR, v);
    upb_inttable_insert(&t, UPB_ENDMSG_SELECTOR, v);
    upb_inttable_insert(&t, UPB_UNKNOWN_SELECTOR, v);
    for(upb_msg_field_begin(&j, m);
        !upb_msg_field_done(&j);
        upb_msg_field_next(&j)) {
      upb_fielddef *f = upb_msg_iter_field(&j);
      /* These calls will assert-fail in upb_table if the value already
       * exists. */
      TRY(UPB_HANDLER_INT32);
      TRY(UPB_HANDLER_INT64)
      TRY(UPB_HANDLER_UINT32)
      TRY(UPB_HANDLER_UINT64)
      TRY(UPB_HANDLER_FLOAT)
      TRY(UPB_HANDLER_DOUBLE)
      TRY(UPB_HANDLER_BOOL)
      TRY(UPB_HANDLER_STARTSTR)
      TRY(UPB_HANDLER_STRING)
      TRY(UPB_HANDLER_ENDSTR)
      TRY(UPB_HANDLER_STARTSUBMSG)
      TRY(UPB_HANDLER_ENDSUBMSG)
      TRY(UPB_HANDLER_STARTSEQ)
      TRY(UPB_HANDLER_ENDSEQ)
    }
    upb_inttable_uninit(&t);
  }
#undef TRY
#endif

  for(upb_msg_oneof_begin(&k, m), i = 0;
      !upb_msg_oneof_done(&k);
      upb_msg_oneof_next(&k), i++) {
    upb_oneofdef *o = upb_msg_iter_oneof(&k);
    o->index = i;
  }

  upb_gfree(fields);
  return true;
}

static void assign_msg_wellknowntype(upb_msgdef *m) {
  const char *name = upb_msgdef_fullname(m);
  if (name == NULL) {
    m->well_known_type = UPB_WELLKNOWN_UNSPECIFIED;
    return;
  }
  if (!strcmp(name, "google.protobuf.Any")) {
    m->well_known_type = UPB_WELLKNOWN_ANY;
  } else if (!strcmp(name, "google.protobuf.Duration")) {
    m->well_known_type = UPB_WELLKNOWN_DURATION;
  } else if (!strcmp(name, "google.protobuf.Timestamp")) {
    m->well_known_type = UPB_WELLKNOWN_TIMESTAMP;
  } else if (!strcmp(name, "google.protobuf.DoubleValue")) {
    m->well_known_type = UPB_WELLKNOWN_DOUBLEVALUE;
  } else if (!strcmp(name, "google.protobuf.FloatValue")) {
    m->well_known_type = UPB_WELLKNOWN_FLOATVALUE;
  } else if (!strcmp(name, "google.protobuf.Int64Value")) {
    m->well_known_type = UPB_WELLKNOWN_INT64VALUE;
  } else if (!strcmp(name, "google.protobuf.UInt64Value")) {
    m->well_known_type = UPB_WELLKNOWN_UINT64VALUE;
  } else if (!strcmp(name, "google.protobuf.Int32Value")) {
    m->well_known_type = UPB_WELLKNOWN_INT32VALUE;
  } else if (!strcmp(name, "google.protobuf.UInt32Value")) {
    m->well_known_type = UPB_WELLKNOWN_UINT32VALUE;
  } else if (!strcmp(name, "google.protobuf.BoolValue")) {
    m->well_known_type = UPB_WELLKNOWN_BOOLVALUE;
  } else if (!strcmp(name, "google.protobuf.StringValue")) {
    m->well_known_type = UPB_WELLKNOWN_STRINGVALUE;
  } else if (!strcmp(name, "google.protobuf.BytesValue")) {
    m->well_known_type = UPB_WELLKNOWN_BYTESVALUE;
  } else if (!strcmp(name, "google.protobuf.Value")) {
    m->well_known_type = UPB_WELLKNOWN_VALUE;
  } else if (!strcmp(name, "google.protobuf.ListValue")) {
    m->well_known_type = UPB_WELLKNOWN_LISTVALUE;
  } else if (!strcmp(name, "google.protobuf.Struct")) {
    m->well_known_type = UPB_WELLKNOWN_STRUCT;
  } else {
    m->well_known_type = UPB_WELLKNOWN_UNSPECIFIED;
  }
}

#if 0
bool _upb_def_validate(upb_def *const*defs, size_t n, upb_status *s) {
  size_t i;

  /* First perform validation, in two passes so we can check that we have a
   * transitive closure without needing to search. */
  for (i = 0; i < n; i++) {
    upb_def *def = defs[i];
    if (upb_def_isfrozen(def)) {
      /* Could relax this requirement if it's annoying. */
      upb_status_seterrmsg(s, "def is already frozen");
      goto err;
    } else if (def->type == UPB_DEF_FIELD) {
      upb_status_seterrmsg(s, "standalone fielddefs can not be frozen");
      goto err;
    } else {
      /* Set now to detect transitive closure in the second pass. */
      def->came_from_user = true;

      if (def->type == UPB_DEF_ENUM &&
          !upb_validate_enumdef(upb_dyncast_enumdef(def), s)) {
        goto err;
      }
    }
  }

  /* Second pass of validation.  Also assign selector bases and indexes, and
   * compact tables. */
  for (i = 0; i < n; i++) {
    upb_def *def = defs[i];
    upb_msgdef *m = upb_dyncast_msgdef_mutable(def);
    upb_enumdef *e = upb_dyncast_enumdef_mutable(def);
    if (m) {
      upb_inttable_compact(&m->itof);
      if (!assign_msg_indices(m, s)) {
        goto err;
      }
      assign_msg_wellknowntype(m);
      /* m->well_known_type = UPB_WELLKNOWN_UNSPECIFIED; */
    } else if (e) {
      upb_inttable_compact(&e->iton);
    }
  }

  return true;

err:
  for (i = 0; i < n; i++) {
    upb_def *def = defs[i];
    def->came_from_user = false;
  }
  UPB_ASSERT(!(s && upb_ok(s)));
  return false;
}
#endif


/* upb_enumdef ****************************************************************/

const char *upb_enumdef_fullname(const upb_enumdef *e) {
  return e->full_name;
}

const char *upb_enumdef_name(const upb_enumdef *e) {
  return shortdefname(e->full_name);
}

#if 0
bool upb_enumdef_addval(upb_enumdef *e, const char *name, int32_t num,
                        upb_status *status) {
  char *name2;

  if (!upb_isident(name, strlen(name), false, status)) {
    return false;
  }

  if (upb_enumdef_ntoiz(e, name, NULL)) {
    upb_status_seterrf(status, "name '%s' is already defined", name);
    return false;
  }

  if (!upb_strtable_insert(&e->ntoi, name, upb_value_int32(num))) {
    upb_status_seterrmsg(status, "out of memory");
    return false;
  }

  if (!upb_inttable_lookup(&e->iton, num, NULL)) {
    name2 = upb_gstrdup(name);
    if (!name2 || !upb_inttable_insert(&e->iton, num, upb_value_cstr(name2))) {
      upb_status_seterrmsg(status, "out of memory");
      upb_strtable_remove(&e->ntoi, name, NULL);
      return false;
    }
  }

  if (upb_enumdef_numvals(e) == 1) {
    bool ok = upb_enumdef_setdefault(e, num, NULL);
    UPB_ASSERT(ok);
  }

  return true;
}
#endif

int32_t upb_enumdef_default(const upb_enumdef *e) {
  UPB_ASSERT(upb_enumdef_iton(e, e->defaultval));
  return e->defaultval;
}

#if 0
bool upb_enumdef_setdefault(upb_enumdef *e, int32_t val, upb_status *s) {
  UPB_ASSERT(!upb_enumdef_isfrozen(e));
  if (!upb_enumdef_iton(e, val)) {
    upb_status_seterrf(s, "number '%d' is not in the enum.", val);
    return false;
  }
  e->defaultval = val;
  return true;
}
#endif

int upb_enumdef_numvals(const upb_enumdef *e) {
  return upb_strtable_count(&e->ntoi);
}

void upb_enum_begin(upb_enum_iter *i, const upb_enumdef *e) {
  /* We iterate over the ntoi table, to account for duplicate numbers. */
  upb_strtable_begin(i, &e->ntoi);
}

void upb_enum_next(upb_enum_iter *iter) { upb_strtable_next(iter); }
bool upb_enum_done(upb_enum_iter *iter) { return upb_strtable_done(iter); }

bool upb_enumdef_ntoi(const upb_enumdef *def, const char *name,
                      size_t len, int32_t *num) {
  upb_value v;
  if (!upb_strtable_lookup2(&def->ntoi, name, len, &v)) {
    return false;
  }
  if (num) *num = upb_value_getint32(v);
  return true;
}

const char *upb_enumdef_iton(const upb_enumdef *def, int32_t num) {
  upb_value v;
  return upb_inttable_lookup32(&def->iton, num, &v) ?
      upb_value_getcstr(v) : NULL;
}

const char *upb_enum_iter_name(upb_enum_iter *iter) {
  return upb_strtable_iter_key(iter);
}

int32_t upb_enum_iter_number(upb_enum_iter *iter) {
  return upb_value_getint32(upb_strtable_iter_value(iter));
}


/* upb_fielddef ***************************************************************/

const char *upb_fielddef_fullname(const upb_fielddef *f) {
  return f->full_name;
}

upb_fieldtype_t upb_fielddef_type(const upb_fielddef *f) {
  switch (f->type_) {
    case UPB_DESCRIPTOR_TYPE_DOUBLE:
      return UPB_TYPE_DOUBLE;
    case UPB_DESCRIPTOR_TYPE_FLOAT:
      return UPB_TYPE_FLOAT;
    case UPB_DESCRIPTOR_TYPE_INT64:
    case UPB_DESCRIPTOR_TYPE_SINT64:
    case UPB_DESCRIPTOR_TYPE_SFIXED64:
      return UPB_TYPE_INT64;
    case UPB_DESCRIPTOR_TYPE_INT32:
    case UPB_DESCRIPTOR_TYPE_SFIXED32:
    case UPB_DESCRIPTOR_TYPE_SINT32:
      return UPB_TYPE_INT32;
    case UPB_DESCRIPTOR_TYPE_UINT64:
    case UPB_DESCRIPTOR_TYPE_FIXED64:
      return UPB_TYPE_UINT64;
    case UPB_DESCRIPTOR_TYPE_UINT32:
    case UPB_DESCRIPTOR_TYPE_FIXED32:
      return UPB_TYPE_UINT32;
    case UPB_DESCRIPTOR_TYPE_ENUM:
      return UPB_TYPE_ENUM;
    case UPB_DESCRIPTOR_TYPE_BOOL:
      return UPB_TYPE_BOOL;
    case UPB_DESCRIPTOR_TYPE_STRING:
      return UPB_TYPE_STRING;
    case UPB_DESCRIPTOR_TYPE_BYTES:
      return UPB_TYPE_BYTES;
    case UPB_DESCRIPTOR_TYPE_GROUP:
    case UPB_DESCRIPTOR_TYPE_MESSAGE:
      return UPB_TYPE_MESSAGE;
  }
  UPB_UNREACHABLE();
}

upb_descriptortype_t upb_fielddef_descriptortype(const upb_fielddef *f) {
  return f->type_;
}

uint32_t upb_fielddef_index(const upb_fielddef *f) {
  return f->index_;
}

upb_label_t upb_fielddef_label(const upb_fielddef *f) {
  return f->label_;
}

uint32_t upb_fielddef_number(const upb_fielddef *f) {
  return f->number_;
}

bool upb_fielddef_isextension(const upb_fielddef *f) {
  return f->is_extension_;
}

bool upb_fielddef_lazy(const upb_fielddef *f) {
  return f->lazy_;
}

bool upb_fielddef_packed(const upb_fielddef *f) {
  return f->packed_;
}

const char *upb_fielddef_name(const upb_fielddef *f) {
  return f->full_name;
}

size_t upb_fielddef_getjsonname(const upb_fielddef *f, char *buf, size_t len) {
  const char *name = upb_fielddef_name(f);
  size_t src, dst = 0;
  bool ucase_next = false;

#define WRITE(byte) \
  ++dst; \
  if (dst < len) buf[dst - 1] = byte; \
  else if (dst == len) buf[dst - 1] = '\0'

  if (!name) {
    WRITE('\0');
    return 0;
  }

  /* Implement the transformation as described in the spec:
   *   1. upper case all letters after an underscore.
   *   2. remove all underscores.
   */
  for (src = 0; name[src]; src++) {
    if (name[src] == '_') {
      ucase_next = true;
      continue;
    }

    if (ucase_next) {
      WRITE(toupper(name[src]));
      ucase_next = false;
    } else {
      WRITE(name[src]);
    }
  }

  WRITE('\0');
  return dst;

#undef WRITE
}

const upb_msgdef *upb_fielddef_containingtype(const upb_fielddef *f) {
  return f->msgdef;
}

const upb_oneofdef *upb_fielddef_containingoneof(const upb_fielddef *f) {
  return f->oneof;
}

static void chkdefaulttype(const upb_fielddef *f, int ctype) {
  UPB_UNUSED(f);
  UPB_UNUSED(ctype);
}

int64_t upb_fielddef_defaultint64(const upb_fielddef *f) {
  chkdefaulttype(f, UPB_TYPE_INT64);
  return f->defaultval.sint;
}

int32_t upb_fielddef_defaultint32(const upb_fielddef *f) {
  chkdefaulttype(f, UPB_TYPE_INT32);
  return f->defaultval.sint;
}

uint64_t upb_fielddef_defaultuint64(const upb_fielddef *f) {
  chkdefaulttype(f, UPB_TYPE_UINT64);
  return f->defaultval.uint;
}

uint32_t upb_fielddef_defaultuint32(const upb_fielddef *f) {
  chkdefaulttype(f, UPB_TYPE_UINT32);
  return f->defaultval.uint;
}

bool upb_fielddef_defaultbool(const upb_fielddef *f) {
  chkdefaulttype(f, UPB_TYPE_BOOL);
  return f->defaultval.uint;
}

float upb_fielddef_defaultfloat(const upb_fielddef *f) {
  chkdefaulttype(f, UPB_TYPE_FLOAT);
  return f->defaultval.flt;
}

double upb_fielddef_defaultdouble(const upb_fielddef *f) {
  chkdefaulttype(f, UPB_TYPE_DOUBLE);
  return f->defaultval.dbl;
}

const char *upb_fielddef_defaultstr(const upb_fielddef *f, size_t *len) {
  str_t *str = f->defaultval.str;
  UPB_ASSERT(upb_fielddef_type(f) == UPB_TYPE_STRING ||
         upb_fielddef_type(f) == UPB_TYPE_BYTES ||
         upb_fielddef_type(f) == UPB_TYPE_ENUM);
  if (len) *len = str->len;
  return str->str;
}

const upb_msgdef *upb_fielddef_msgsubdef(const upb_fielddef *f) {
  UPB_ASSERT(upb_fielddef_type(f) == UPB_TYPE_MESSAGE);
  return f->sub.msgdef;
}

const upb_enumdef *upb_fielddef_enumsubdef(const upb_fielddef *f) {
  UPB_ASSERT(upb_fielddef_type(f) == UPB_TYPE_ENUM);
  return f->sub.enumdef;
}

bool upb_fielddef_issubmsg(const upb_fielddef *f) {
  return upb_fielddef_type(f) == UPB_TYPE_MESSAGE;
}

bool upb_fielddef_isstring(const upb_fielddef *f) {
  return upb_fielddef_type(f) == UPB_TYPE_STRING ||
         upb_fielddef_type(f) == UPB_TYPE_BYTES;
}

bool upb_fielddef_isseq(const upb_fielddef *f) {
  return upb_fielddef_label(f) == UPB_LABEL_REPEATED;
}

bool upb_fielddef_isprimitive(const upb_fielddef *f) {
  return !upb_fielddef_isstring(f) && !upb_fielddef_issubmsg(f);
}

bool upb_fielddef_ismap(const upb_fielddef *f) {
  return upb_fielddef_isseq(f) && upb_fielddef_issubmsg(f) &&
         upb_msgdef_mapentry(upb_fielddef_msgsubdef(f));
}

bool upb_fielddef_hassubdef(const upb_fielddef *f) {
  return upb_fielddef_issubmsg(f) || upb_fielddef_type(f) == UPB_TYPE_ENUM;
}

static bool between(int32_t x, int32_t low, int32_t high) {
  return x >= low && x <= high;
}

bool upb_fielddef_checklabel(int32_t label) { return between(label, 1, 3); }
bool upb_fielddef_checktype(int32_t type) { return between(type, 1, 11); }
bool upb_fielddef_checkintfmt(int32_t fmt) { return between(fmt, 1, 3); }

bool upb_fielddef_checkdescriptortype(int32_t type) {
  return between(type, 1, 18);
}

/* upb_msgdef *****************************************************************/

const char *upb_msgdef_fullname(const upb_msgdef *m) {
  return m->full_name;
}

const char *upb_msgdef_name(const upb_msgdef *m) {
  return shortdefname(m->full_name);
}

upb_syntax_t upb_msgdef_syntax(const upb_msgdef *m) {
  return m->file->syntax;
}

#if 0
/* Helper: check that the field |f| is safe to add to msgdef |m|. Set an error
 * on status |s| and return false if not. */
static bool check_field_add(const upb_msgdef *m, const upb_fielddef *f,
                            upb_status *s) {
  if (upb_fielddef_containingtype(f) != NULL) {
    upb_status_seterrmsg(s, "fielddef already belongs to a message");
    return false;
  return true;
}

bool upb_msgdef_addfield(upb_msgdef *m, upb_fielddef *f, const void *ref_donor,
                         upb_status *s) {
  /* TODO: extensions need to have a separate namespace, because proto2 allows a
   * top-level extension (ie. one not in any package) to have the same name as a
   * field from the message.
   *
   * This also implies that there needs to be a separate lookup-by-name method
   * for extensions.  It seems desirable for iteration to return both extensions
   * and non-extensions though.
   *
   * We also need to validate that the field number is in an extension range iff
   * it is an extension.
   *
   * This method is idempotent. Check if |f| is already part of this msgdef and
   * return immediately if so. */
  if (upb_fielddef_containingtype(f) == m) {
    if (ref_donor) upb_fielddef_unref(f, ref_donor);
    return true;
  }

  /* Check constraints for all fields before performing any action. */
  if (!check_field_add(m, f, s)) {
    return false;
  } else if (upb_fielddef_containingoneof(f) != NULL) {
    /* Fields in a oneof can only be added by adding the oneof to the msgdef. */
    upb_status_seterrmsg(s, "fielddef is part of a oneof");
    return false;
  }

  /* Constraint checks ok, perform the action. */
  add_field(m, f, ref_donor);
  return true;
}

bool upb_msgdef_addoneof(upb_msgdef *m, upb_oneofdef *o, const void *ref_donor,
                         upb_status *s) {
  upb_oneof_iter it;

  /* Check various conditions that would prevent this oneof from being added. */
  if (upb_oneofdef_containingtype(o)) {
    upb_status_seterrmsg(s, "oneofdef already belongs to a message");
    return false;
  } else if (upb_oneofdef_name(o) == NULL) {
    upb_status_seterrmsg(s, "oneofdef name was not set");
    return false;
  } else if (upb_strtable_lookup(&m->ntof, upb_oneofdef_name(o), NULL)) {
    upb_status_seterrmsg(s, "name conflicts with existing field or oneof");
    return false;
  }

  /* Check that all of the oneof's fields do not conflict with names or numbers
   * of fields already in the message. */
  for (upb_oneof_begin(&it, o); !upb_oneof_done(&it); upb_oneof_next(&it)) {
    const upb_fielddef *f = upb_oneof_iter_field(&it);
    if (!check_field_add(m, f, s)) {
      return false;
    }
  }

  /* Everything checks out -- commit now. */

  /* Add oneof itself first. */
  o->parent = m;
  upb_strtable_insert(&m->ntof, upb_oneofdef_name(o), upb_value_ptr(o));
  upb_ref2(o, m);
  upb_ref2(m, o);

  /* Add each field of the oneof directly to the msgdef. */
  for (upb_oneof_begin(&it, o); !upb_oneof_done(&it); upb_oneof_next(&it)) {
    upb_fielddef *f = upb_oneof_iter_field(&it);
    add_field(m, f, NULL);
  }

  if (ref_donor) upb_oneofdef_unref(o, ref_donor);

  return true;
}
#endif

const upb_fielddef *upb_msgdef_itof(const upb_msgdef *m, uint32_t i) {
  upb_value val;
  return upb_inttable_lookup32(&m->itof, i, &val) ?
      upb_value_getptr(val) : NULL;
}

const upb_fielddef *upb_msgdef_ntof(const upb_msgdef *m, const char *name,
                                    size_t len) {
  upb_value val;

  if (!upb_strtable_lookup2(&m->ntof, name, len, &val)) {
    return NULL;
  }

  return upb_trygetfield(upb_value_getptr(val));
}

const upb_oneofdef *upb_msgdef_ntoo(const upb_msgdef *m, const char *name,
                                    size_t len) {
  upb_value val;

  if (!upb_strtable_lookup2(&m->ntof, name, len, &val)) {
    return NULL;
  }

  return upb_trygetoneof(upb_value_getptr(val));
}

bool upb_msgdef_lookupname(const upb_msgdef *m, const char *name, size_t len,
                           const upb_fielddef **f, const upb_oneofdef **o) {
  upb_value val;

  if (!upb_strtable_lookup2(&m->ntof, name, len, &val)) {
    return false;
  }

  *o = upb_trygetoneof(upb_value_getptr(val));
  *f = upb_trygetfield(upb_value_getptr(val));
  UPB_ASSERT((*o != NULL) ^ (*f != NULL));  /* Exactly one of the two should be set. */
  return true;
}

int upb_msgdef_numfields(const upb_msgdef *m) {
  /* The number table contains only fields. */
  return upb_inttable_count(&m->itof);
}

int upb_msgdef_numoneofs(const upb_msgdef *m) {
  /* The name table includes oneofs, and the number table does not. */
  return upb_strtable_count(&m->ntof) - upb_inttable_count(&m->itof);
}

bool upb_msgdef_mapentry(const upb_msgdef *m) {
  return m->map_entry;
}

upb_wellknowntype_t upb_msgdef_wellknowntype(const upb_msgdef *m) {
  return m->well_known_type;
}

bool upb_msgdef_isnumberwrapper(const upb_msgdef *m) {
  upb_wellknowntype_t type = upb_msgdef_wellknowntype(m);
  return type >= UPB_WELLKNOWN_DOUBLEVALUE &&
         type <= UPB_WELLKNOWN_UINT32VALUE;
}

void upb_msg_field_begin(upb_msg_field_iter *iter, const upb_msgdef *m) {
  upb_inttable_begin(iter, &m->itof);
}

void upb_msg_field_next(upb_msg_field_iter *iter) { upb_inttable_next(iter); }

bool upb_msg_field_done(const upb_msg_field_iter *iter) {
  return upb_inttable_done(iter);
}

upb_fielddef *upb_msg_iter_field(const upb_msg_field_iter *iter) {
  return (upb_fielddef*)upb_value_getptr(upb_inttable_iter_value(iter));
}

void upb_msg_field_iter_setdone(upb_msg_field_iter *iter) {
  upb_inttable_iter_setdone(iter);
}

void upb_msg_oneof_begin(upb_msg_oneof_iter *iter, const upb_msgdef *m) {
  upb_strtable_begin(iter, &m->ntof);
  /* We need to skip past any initial fields. */
  while (!upb_strtable_done(iter) &&
         !upb_isoneof(upb_value_getptr(upb_strtable_iter_value(iter)))) {
    upb_strtable_next(iter);
  }
}

void upb_msg_oneof_next(upb_msg_oneof_iter *iter) {
  /* We need to skip past fields to return only oneofs. */
  do {
    upb_strtable_next(iter);
  } while (!upb_strtable_done(iter) &&
           !upb_isoneof(upb_value_getptr(upb_strtable_iter_value(iter))));
}

bool upb_msg_oneof_done(const upb_msg_oneof_iter *iter) {
  return upb_strtable_done(iter);
}

upb_oneofdef *upb_msg_iter_oneof(const upb_msg_oneof_iter *iter) {
  return (upb_oneofdef*)upb_value_getptr(upb_strtable_iter_value(iter));
}

void upb_msg_oneof_iter_setdone(upb_msg_oneof_iter *iter) {
  upb_strtable_iter_setdone(iter);
}

/* upb_oneofdef ***************************************************************/

const char *upb_oneofdef_name(const upb_oneofdef *o) { return o->name; }

const upb_msgdef *upb_oneofdef_containingtype(const upb_oneofdef *o) {
  return o->parent;
}

int upb_oneofdef_numfields(const upb_oneofdef *o) {
  return upb_strtable_count(&o->ntof);
}

uint32_t upb_oneofdef_index(const upb_oneofdef *o) {
  return o->index;
}

#if 0
bool upb_oneofdef_addfield(upb_oneofdef *o, upb_fielddef *f,
                           const void *ref_donor,
                           upb_status *s) {
  /* This method is idempotent. Check if |f| is already part of this oneofdef
   * and return immediately if so. */
  if (upb_fielddef_containingoneof(f) == o) {
    return true;
  }

  /* The field must have an OPTIONAL label. */
  if (upb_fielddef_label(f) != UPB_LABEL_OPTIONAL) {
    upb_status_seterrmsg(s, "fields in oneof must have OPTIONAL label");
    return false;
  }

  /* Check that no field with this name or number exists already in the oneof.
   * Also check that the field is not already part of a oneof. */
  if (upb_fielddef_name(f) == NULL || upb_fielddef_number(f) == 0) {
    upb_status_seterrmsg(s, "field name or number were not set");
    return false;
  } else if (upb_oneofdef_itof(o, upb_fielddef_number(f)) ||
             upb_oneofdef_ntofz(o, upb_fielddef_name(f))) {
    upb_status_seterrmsg(s, "duplicate field name or number");
    return false;
  } else if (upb_fielddef_containingoneof(f) != NULL) {
    upb_status_seterrmsg(s, "fielddef already belongs to a oneof");
    return false;
  }

  /* We allow adding a field to the oneof either if the field is not part of a
   * msgdef, or if it is and we are also part of the same msgdef. */
  if (o->parent == NULL) {
    /* If we're not in a msgdef, the field cannot be either. Otherwise we would
     * need to magically add this oneof to a msgdef to remain consistent, which
     * is surprising behavior. */
    if (upb_fielddef_containingtype(f) != NULL) {
      upb_status_seterrmsg(s, "fielddef already belongs to a message, but "
                              "oneof does not");
      return false;
    }
  } else {
    /* If we're in a msgdef, the user can add fields that either aren't in any
     * msgdef (in which case they're added to our msgdef) or already a part of
     * our msgdef. */
    if (upb_fielddef_containingtype(f) != NULL &&
        upb_fielddef_containingtype(f) != o->parent) {
      upb_status_seterrmsg(s, "fielddef belongs to a different message "
                              "than oneof");
      return false;
    }
  }

  /* Commit phase. First add the field to our parent msgdef, if any, because
   * that may fail; then add the field to our own tables. */

  if (o->parent != NULL && upb_fielddef_containingtype(f) == NULL) {
    if (!upb_msgdef_addfield((upb_msgdef*)o->parent, f, NULL, s)) {
      return false;
    }
  }

  f->oneof = o;
  upb_inttable_insert(&o->itof, upb_fielddef_number(f), upb_value_ptr(f));
  upb_strtable_insert(&o->ntof, upb_fielddef_name(f), upb_value_ptr(f));
  upb_ref2(f, o);
  upb_ref2(o, f);
  if (ref_donor) upb_fielddef_unref(f, ref_donor);

  return true;
}
#endif

const upb_fielddef *upb_oneofdef_ntof(const upb_oneofdef *o,
                                      const char *name, size_t length) {
  upb_value val;
  return upb_strtable_lookup2(&o->ntof, name, length, &val) ?
      upb_value_getptr(val) : NULL;
}

const upb_fielddef *upb_oneofdef_itof(const upb_oneofdef *o, uint32_t num) {
  upb_value val;
  return upb_inttable_lookup32(&o->itof, num, &val) ?
      upb_value_getptr(val) : NULL;
}

void upb_oneof_begin(upb_oneof_iter *iter, const upb_oneofdef *o) {
  upb_inttable_begin(iter, &o->itof);
}

void upb_oneof_next(upb_oneof_iter *iter) {
  upb_inttable_next(iter);
}

bool upb_oneof_done(upb_oneof_iter *iter) {
  return upb_inttable_done(iter);
}

upb_fielddef *upb_oneof_iter_field(const upb_oneof_iter *iter) {
  return (upb_fielddef*)upb_value_getptr(upb_inttable_iter_value(iter));
}

void upb_oneof_iter_setdone(upb_oneof_iter *iter) {
  upb_inttable_iter_setdone(iter);
}

/* upb_filedef ****************************************************************/

const char *upb_filedef_name(const upb_filedef *f) {
  return f->name;
}

const char *upb_filedef_package(const upb_filedef *f) {
  return f->package;
}

const char *upb_filedef_phpprefix(const upb_filedef *f) {
  return f->phpprefix;
}

const char *upb_filedef_phpnamespace(const upb_filedef *f) {
  return f->phpnamespace;
}

upb_syntax_t upb_filedef_syntax(const upb_filedef *f) {
  return f->syntax;
}

int upb_filedef_msgcount(const upb_filedef *f) {
  return f->msgcount;
}

int upb_filedef_depcount(const upb_filedef *f) {
  return f->depcount;
}

int upb_filedef_enumcount(const upb_filedef *f) {
  return f->enumcount;
}

const upb_filedef *upb_filedef_dep(const upb_filedef *f, int i) {
  return i < 0 || i >= f->depcount ? NULL : f->deps[i];
}

const upb_msgdef *upb_filedef_msg(const upb_filedef *f, int i) {
  return i < 0 || i >= f->msgcount ? NULL : f->msgs[i];
}

const upb_enumdef *upb_filedef_enum(const upb_filedef *f, int i) {
  return i < 0 || i >= f->enumcount ? NULL : f->enums[i];
}

void upb_symtab_free(upb_symtab *s) {
  upb_arena_uninit(&s->arena);
  upb_gfree(s);
}

upb_symtab *upb_symtab_new() {
  upb_symtab *s = upb_gmalloc(sizeof(*s));
  upb_alloc *alloc;

  if (!s) {
    return NULL;
  }

  upb_arena_init(&s->arena);
  alloc = upb_arena_alloc(&s->arena);

  if (!upb_strtable_init2(&s->syms, UPB_CTYPE_CONSTPTR, alloc) ||
      !upb_strtable_init2(&s->files, UPB_CTYPE_CONSTPTR, alloc)) {
    upb_arena_uninit(&s->arena);
    upb_gfree(s);
    s = NULL;
  }
  return s;
}

const upb_msgdef *upb_symtab_lookupmsg(const upb_symtab *s, const char *sym) {
  upb_value v;
  return upb_strtable_lookup(&s->syms, sym, &v) ?
      unpack_def(v, UPB_DEFTYPE_MSG) : NULL;
}

const upb_msgdef *upb_symtab_lookupmsg2(const upb_symtab *s, const char *sym,
                                        size_t len) {
  upb_value v;
  return upb_strtable_lookup2(&s->syms, sym, len, &v) ?
      unpack_def(v, UPB_DEFTYPE_MSG) : NULL;
}

const upb_enumdef *upb_symtab_lookupenum(const upb_symtab *s, const char *sym) {
  upb_value v;
  return upb_strtable_lookup(&s->syms, sym, &v) ?
      unpack_def(v, UPB_DEFTYPE_ENUM) : NULL;
}

#if 0
/* Given a symbol and the base symbol inside which it is defined, find the
 * symbol's definition in t. */
static upb_def *upb_resolvename(const upb_strtable *t,
                                const char *base, const char *sym) {
  if(strlen(sym) == 0) return NULL;
  if(sym[0] == '.') {
    /* Symbols starting with '.' are absolute, so we do a single lookup.
     * Slice to omit the leading '.' */
    upb_value v;
    return upb_strtable_lookup(t, sym + 1, &v) ? upb_value_getptr(v) : NULL;
  } else {
    /* Remove components from base until we find an entry or run out.
     * TODO: This branch is totally broken, but currently not used. */
    (void)base;
    UPB_ASSERT(false);
    return NULL;
  }
}

const upb_def *upb_symtab_resolve(const upb_symtab *s, const char *base,
                                  const char *sym) {
  upb_def *ret = upb_resolvename(&s->syms, base, sym);
  return ret;
}

  /* Now using the table, resolve symbolic references for subdefs. */
  upb_strtable_begin(&iter, &addtab);
  for (; !upb_strtable_done(&iter); upb_strtable_next(&iter)) {
    const char *base;
    upb_def *def = upb_value_getptr(upb_strtable_iter_value(&iter));
    upb_msgdef *m = upb_dyncast_msgdef_mutable(def);
    upb_msg_field_iter j;

    if (!m) continue;
    /* Type names are resolved relative to the message in which they appear. */
    base = upb_msgdef_fullname(m);

    for(upb_msg_field_begin(&j, m);
        !upb_msg_field_done(&j);
        upb_msg_field_next(&j)) {
      upb_fielddef *f = upb_msg_iter_field(&j);
      const char *name = upb_fielddef_subdefname(f);
      if (name && !upb_fielddef_subdef(f)) {
        /* Try the lookup in the current set of to-be-added defs first. If not
         * there, try existing defs. */
        upb_def *subdef = upb_resolvename(&addtab, base, name);
        if (subdef == NULL) {
          subdef = upb_resolvename(&s->syms, base, name);
        }
        if (subdef == NULL) {
          upb_status_seterrf(
              status, "couldn't resolve name '%s' in message '%s'", name, base);
          goto err;
        } else if (!upb_fielddef_setsubdef(f, subdef, status)) {
          goto err;
        }
      }
    }
  }
#endif


/* Code to build defs from descriptor protos. *********************************/

/* There is a question of how much validation to do here.  It will be difficult
 * to perfectly match the amount of validation performed by proto2.  But since
 * this code is used to directly build defs from Ruby (for example) we do need
 * to validate important constraints like uniqueness of names and numbers. */

#define CHK(x) if (!(x)) return false
#define CHK_OOM(x) if (!(x)) { upb_upberr_setoom(ctx->status); return false; }

typedef struct {
  const upb_symtab *symtab;
  const upb_filedef *file;  /* File we are building. */
  upb_alloc *alloc;    /* Allocate defs here. */
  upb_alloc *tmp;      /* Alloc for addtab and any other tmp data. */
  upb_strtable *addtab;  /* full_name -> packed def ptr for new defs. */
  upb_status *status;  /* Record errors here. */
} symtab_addctx;

static const char *makefullname(const char *prefix, upb_stringview name) {
  return NULL;
}

static bool symtab_add(const symtab_addctx *ctx, const char *name,
                       upb_value v) {
  upb_value tmp;
  if (upb_strtable_lookup(ctx->addtab, name, &tmp) ||
      upb_strtable_lookup(&ctx->symtab->syms, name, &tmp)) {
    upb_status_seterrf(ctx->status, "Duplicate symbol '%s'", name);
    return false;
  }

  CHK_OOM(upb_strtable_insert3(ctx->addtab, name, strlen(name), v, ctx->tmp));
  return true;
}

static bool create_oneofdef(
    const symtab_addctx *ctx, upb_msgdef *m,
    const google_protobuf_OneofDescriptorProto *oneof_proto) {
  upb_oneofdef *o = upb_malloc(ctx->alloc, sizeof(*o));
  upb_stringview name = google_protobuf_OneofDescriptorProto_name(oneof_proto);
  upb_value o_ptr = upb_value_ptr(o);

  CHK_OOM(o);
  CHK_OOM(upb_inttable_init2(&o->itof, UPB_CTYPE_PTR, ctx->alloc));
  CHK_OOM(upb_strtable_init2(&o->ntof, UPB_CTYPE_PTR, ctx->alloc));

  o->index = upb_strtable_count(&m->ntof);
  o->name = upb_strdup2(name.data, name.size, ctx->alloc);
  o->parent = m;

  CHK_OOM(upb_strtable_insert3(&m->ntof, name.data, name.size, o_ptr,
                               ctx->alloc));

  return true;
}

static bool parse_default(const char *str, size_t len, upb_fielddef *f) {
  char *end;
  switch (upb_fielddef_type(f)) {
    case UPB_TYPE_INT32: {
      long val = strtol(str, &end, 0);
      CHK(val <= INT32_MAX && val >= INT32_MIN && errno != ERANGE && !*end);
      f->defaultval.sint = val;
      break;
    }
    case UPB_TYPE_ENUM:
    case UPB_TYPE_INT64: {
      /* XXX: Need to write our own strtoll, since it's not available in c89. */
      long long val = strtol(str, &end, 0);
      CHK(val <= INT64_MAX && val >= INT64_MIN && errno != ERANGE && !*end);
      f->defaultval.sint = val;
      break;
    }
    case UPB_TYPE_UINT32: {
      unsigned long val = strtoul(str, &end, 0);
      CHK(val <= UINT32_MAX && errno != ERANGE && !*end);
      f->defaultval.uint = val;
      break;
    }
    case UPB_TYPE_UINT64: {
      /* XXX: Need to write our own strtoull, since it's not available in c89. */
      unsigned long long val = strtoul(str, &end, 0);
      CHK(val <= UINT64_MAX && errno != ERANGE && !*end);
      f->defaultval.uint = val;
      break;
    }
    case UPB_TYPE_DOUBLE: {
      double val = strtod(str, &end);
      CHK(errno != ERANGE && !*end);
      f->defaultval.dbl = val;
      break;
    }
    case UPB_TYPE_FLOAT: {
      /* XXX: Need to write our own strtof, since it's not available in c89. */
      float val = strtod(str, &end);
      CHK(errno != ERANGE && !*end);
      f->defaultval.dbl = val;
      break;
    }
    case UPB_TYPE_BOOL: {
      if (strcmp(str, "false") == 0) {
        f->defaultval.boolean = false;
      } else if (strcmp(str, "true") == 0) {
        f->defaultval.boolean = true;
      } else {
        return false;
      }
    }
    case UPB_TYPE_STRING:
      f->defaultval.str = newstr(str, len);
      break;
    case UPB_TYPE_BYTES:
      /* XXX: need to interpret the C-escaped value. */
      f->defaultval.str = newstr(str, len);
    case UPB_TYPE_MESSAGE:
      /* Should not have a default value. */
      return false;
  }
  return true;
}

static bool create_fielddef(
    const symtab_addctx *ctx, const char *prefix, upb_msgdef *m,
    const google_protobuf_FieldDescriptorProto *field_proto) {
  upb_alloc *alloc = ctx->alloc;
  upb_fielddef *f = upb_malloc(ctx->alloc, sizeof(*f));
  const google_protobuf_FieldOptions *options;
  upb_stringview defaultval, name;
  upb_value packed_v = pack_def(f, UPB_DEFTYPE_FIELD);
  upb_value v = upb_value_constptr(f);
  const char *shortname;

  CHK_OOM(f);
  memset(f, 0, sizeof(*f));

  name = google_protobuf_FieldDescriptorProto_name(field_proto);

  /* TODO(haberman): use hazzers. */
  if (name.size == 0 || f->number_ == 0) {
    upb_status_seterrmsg(ctx->status, "field name or number were not set");
    return false;
  }

  CHK(upb_isident(name, false, ctx->status));

  f->full_name = makefullname(prefix, name);
  f->type_ = (int)google_protobuf_FieldDescriptorProto_type(field_proto);
  f->label_ = (int)google_protobuf_FieldDescriptorProto_label(field_proto);
  f->number_ = google_protobuf_FieldDescriptorProto_number(field_proto);
  shortname = shortdefname(f->full_name);

  if (f->number_ == 0 || f->number_ > UPB_MAX_FIELDNUMBER) {
    upb_status_seterrf(ctx->status, "invalid field number (%u)", f->number_);
    return false;
  }
#if 0
  //f->oneof
  //f->sub
#endif

  defaultval = google_protobuf_FieldDescriptorProto_default_value(field_proto);

  if (defaultval.data) {
    CHK(parse_default(defaultval.data, defaultval.size, f));
  }

  options = google_protobuf_FieldDescriptorProto_options(field_proto);

  if (options) {
    f->lazy_ = google_protobuf_FieldOptions_lazy(options);
    f->packed_ = google_protobuf_FieldOptions_packed(options);
  }

  if (m) {
    /* direct message field. */
    if (!upb_strtable_insert3(&m->ntof, name.data, name.size, packed_v, alloc)) {
      upb_status_seterrf(ctx->status, "duplicate name (%s)", shortname);
      return false;
    }

    if (!upb_inttable_insert2(&m->itof, f->number_, v, alloc)) {
      upb_status_seterrf(ctx->status, "duplicate field number (%u)", f->number_);
      return false;
    }

    f->msgdef = m;
    f->is_extension_ = false;
  } else {
    /* extension field. */
    f->is_extension_ = true;
  }

  return true;
}

static bool create_enumdef(
    const symtab_addctx *ctx, const char *prefix,
    const google_protobuf_EnumDescriptorProto *enum_proto) {
  upb_enumdef *e = upb_malloc(ctx->alloc, sizeof(*e));
  const upb_array *arr;
  upb_stringview name;
  size_t i;

  name = google_protobuf_EnumDescriptorProto_name(enum_proto);
  CHK(upb_isident(name, false, ctx->status));

  e->full_name = makefullname(prefix, name);
  e->defaultval = 0;

  CHK_OOM(e);
  CHK_OOM(upb_strtable_init2(&e->ntoi, UPB_CTYPE_INT32, ctx->alloc));
  CHK_OOM(upb_inttable_init2(&e->iton, UPB_CTYPE_CSTR, ctx->alloc));

  arr = google_protobuf_EnumDescriptorProto_value(enum_proto);

  for (i = 0; i < upb_array_size(arr); i++) {
    const google_protobuf_EnumValueDescriptorProto *value =
        upb_msgval_getptr(upb_array_get(arr, i));
    upb_stringview name = google_protobuf_EnumValueDescriptorProto_name(value);
    char *name2 = upb_strdup2(name.data, name.size, ctx->alloc);
    int32_t num = google_protobuf_EnumValueDescriptorProto_number(value);
    upb_value v = upb_value_int32(num);

    CHK_OOM(name2 && upb_strtable_insert(&e->ntoi, name2, v));

    if (!upb_inttable_lookup(&e->iton, num, NULL)) {
      CHK_OOM(upb_inttable_insert(&e->iton, num, upb_value_cstr(name2)));
    }
  }

  return true;
}

static bool create_msgdef(const symtab_addctx *ctx, const char *prefix,
                          const google_protobuf_DescriptorProto *msg_proto) {
  upb_msgdef *m = upb_malloc(ctx->alloc, sizeof(upb_msgdef));
  const google_protobuf_MessageOptions *options;
  const upb_array *arr;
  size_t i;
  upb_stringview name;

  CHK_OOM(m);
  CHK_OOM(upb_inttable_init2(&m->itof, UPB_CTYPE_PTR, ctx->alloc));
  CHK_OOM(upb_strtable_init2(&m->ntof, UPB_CTYPE_PTR, ctx->alloc));

  name = google_protobuf_DescriptorProto_name(msg_proto);
  CHK(upb_isident(name, false, ctx->status));

  m->file = ctx->file;
  m->full_name = makefullname(prefix, name);
  m->map_entry = false;

  options = google_protobuf_DescriptorProto_options(msg_proto);

  if (options) {
    m->map_entry = google_protobuf_MessageOptions_map_entry(options);
  }

  arr = google_protobuf_DescriptorProto_oneof_decl(msg_proto);

  for (i = 0; i < upb_array_size(arr); i++) {
    const google_protobuf_OneofDescriptorProto *oneof_proto =
        upb_msgval_getptr(upb_array_get(arr, i));
    CHK(create_oneofdef(ctx, m, oneof_proto));
  }

  arr = google_protobuf_DescriptorProto_field(msg_proto);

  for (i = 0; i < upb_array_size(arr); i++) {
    const google_protobuf_FieldDescriptorProto *field_proto =
        upb_msgval_getptr(upb_array_get(arr, i));
    CHK(create_fielddef(ctx, m->full_name, m, field_proto));
  }

  CHK(assign_msg_indices(m, ctx->status));
  assign_msg_wellknowntype(m);
  symtab_add(ctx, m->full_name, pack_def(m, UPB_DEFTYPE_MSG));

  /* This message is built.  Now build nested messages and enums. */

  arr = google_protobuf_DescriptorProto_enum_type(msg_proto);

  for (i = 0; i < upb_array_size(arr); i++) {
    const google_protobuf_EnumDescriptorProto *enum_proto =
        upb_msgval_getptr(upb_array_get(arr, i));
    CHK(create_enumdef(ctx, m->full_name, enum_proto));
  }

  arr = google_protobuf_DescriptorProto_nested_type(msg_proto);

  for (i = 0; i < upb_array_size(arr); i++) {
    const google_protobuf_DescriptorProto *msg_proto2 =
        upb_msgval_getptr(upb_array_get(arr, i));
    CHK(create_msgdef(ctx, m->full_name, msg_proto2));
  }

  return true;
}

static char* strviewdup(const symtab_addctx *ctx, upb_stringview view) {
  if (view.size == 0) {
    return NULL;
  }
  return upb_strdup2(view.data, view.size, ctx->alloc);
}

static bool build_filedef(
    const symtab_addctx *ctx, upb_filedef *file,
    const google_protobuf_FileDescriptorProto *file_proto) {
  const google_protobuf_FileOptions *file_options_proto;
  upb_strtable_iter iter;
  const upb_array *arr;
  size_t i, n;
  upb_stringview syntax, package;

  package = google_protobuf_FileDescriptorProto_package(file_proto);
  CHK(upb_isident(package, true, ctx->status));

  file->name =
      strviewdup(ctx, google_protobuf_FileDescriptorProto_name(file_proto));
  file->package = strviewdup(ctx, package);
  file->phpprefix = NULL;
  file->phpnamespace = NULL;

  syntax = google_protobuf_FileDescriptorProto_syntax(file_proto);

  if (upb_stringview_eql(syntax, upb_stringview_makez("proto2"))) {
    file->syntax = UPB_SYNTAX_PROTO2;
  } else if (upb_stringview_eql(syntax, upb_stringview_makez("proto3"))) {
    file->syntax = UPB_SYNTAX_PROTO3;
  } else {
    upb_status_seterrf(ctx->status, "Invalid syntax '%s'", syntax);
    return false;
  }

  /* Read options. */

  file_options_proto = google_protobuf_FileDescriptorProto_options(file_proto);

  if (file_options_proto) {
    file->phpprefix = strviewdup(
        ctx, google_protobuf_FileOptions_php_class_prefix(file_options_proto));
    file->phpnamespace = strviewdup(
        ctx, google_protobuf_FileOptions_php_namespace(file_options_proto));
  }

  /* Resolve dependencies. */

  arr = google_protobuf_FileDescriptorProto_dependency(file_proto);
  n = upb_array_size(arr);
  file->deps = upb_malloc(ctx->alloc, sizeof(*file->deps) * n) ;
  CHK_OOM(file->deps);
  for (i = 0; i < n; i++) {
    upb_stringview dep_name = upb_msgval_getstr(upb_array_get(arr, i));
    upb_value v;
    if (!upb_strtable_lookup2(&ctx->symtab->files, dep_name.data,
                              dep_name.size, &v)) {
      upb_status_seterrf(ctx->status,
                         "Depends on file '%s', but it has not been loaded",
                         dep_name.data);
      return false;
    }
    file->deps[i] = upb_value_getconstptr(v);
  }

  /* Create messages. */

  arr = google_protobuf_FileDescriptorProto_message_type(file_proto);
  for (i = 0; i < upb_array_size(arr); i++) {
    const google_protobuf_DescriptorProto *msg_proto =
        upb_msgval_getptr(upb_array_get(arr, i));
    CHK(create_msgdef(ctx, file->package, msg_proto));
  }

  /* Create enums. */

  arr = google_protobuf_FileDescriptorProto_enum_type(file_proto);
  for (i = 0; i < upb_array_size(arr); i++) {
    const google_protobuf_EnumDescriptorProto *enum_proto =
        upb_msgval_getptr(upb_array_get(arr, i));
    CHK(create_enumdef(ctx, file->package, enum_proto));
  }

  /* Now that all names are in the table, resolve references. */
  upb_strtable_begin(&iter, ctx->addtab);
  for (; !upb_strtable_done(&iter); upb_strtable_next(&iter)) {
  }

  return true;
}

static bool upb_symtab_addtotabs(upb_symtab *s, symtab_addctx *ctx,
                                 upb_status *status) {
  const upb_filedef *file = ctx->file;
  upb_alloc *alloc = upb_arena_alloc(&s->arena);
  upb_strtable_iter iter;

  CHK_OOM(upb_strtable_insert3(&s->files, file->name, strlen(file->name),
                               upb_value_constptr(file), alloc));

  upb_strtable_begin(&iter, ctx->addtab);
  for (; !upb_strtable_done(&iter); upb_strtable_next(&iter)) {
    const char *key = upb_strtable_iter_key(&iter);
    size_t keylen = upb_strtable_iter_keylength(&iter);
    upb_value value = upb_strtable_iter_value(&iter);
    CHK_OOM(upb_strtable_insert3(&s->syms, key, keylen, value, alloc));
  }

  return true;
}

bool upb_symtab_addfile(upb_symtab *s,
                        const google_protobuf_FileDescriptorProto *file_proto,
                        upb_status *status) {
  upb_arena tmparena;
  upb_strtable addtab;
  symtab_addctx ctx;
  upb_alloc *alloc = upb_arena_alloc(&s->arena);
  upb_filedef *file = upb_malloc(alloc, sizeof(*file));
  bool ok;

  ctx.file = file;
  ctx.alloc = alloc;
  ctx.tmp = upb_arena_alloc(&tmparena);
  ctx.addtab = &addtab;
  ctx.status = status;

  upb_arena_init(&tmparena);

  ok = file &&
      upb_strtable_init2(&addtab, UPB_CTYPE_PTR, ctx.tmp) &&
      build_filedef(&ctx, file, file_proto) &&
      upb_symtab_addtotabs(s, &ctx, status);

  upb_arena_uninit(&tmparena);
  return ok;
}

#undef CHK
#undef CHK_OOM
