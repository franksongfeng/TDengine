/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3 * or later ("AGPL"), as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "indexComm.h"
#include "index.h"
#include "indexInt.h"
#include "tcoding.h"
#include "tcompare.h"
#include "tdataformat.h"
#include "ttypes.h"
#include "tvariant.h"

#define INDEX_DATA_BOOL_NULL      0x02
#define INDEX_DATA_TINYINT_NULL   0x80
#define INDEX_DATA_SMALLINT_NULL  0x8000
#define INDEX_DATA_INT_NULL       0x80000000LL
#define INDEX_DATA_BIGINT_NULL    0x8000000000000000LL
#define INDEX_DATA_TIMESTAMP_NULL TSDB_DATA_BIGINT_NULL

#define INDEX_DATA_FLOAT_NULL    0x7FF00000            // it is an NAN
#define INDEX_DATA_DOUBLE_NULL   0x7FFFFF0000000000LL  // an NAN
#define INDEX_DATA_NCHAR_NULL    0xFFFFFFFF
#define INDEX_DATA_BINARY_NULL   0xFF
#define INDEX_DATA_JSON_NULL     0xFFFFFFFF
#define INDEX_DATA_JSON_null     0xFFFFFFFE
#define INDEX_DATA_JSON_NOT_NULL 0x01

#define INDEX_DATA_UTINYINT_NULL  0xFF
#define INDEX_DATA_USMALLINT_NULL 0xFFFF
#define INDEX_DATA_UINT_NULL      0xFFFFFFFF
#define INDEX_DATA_UBIGINT_NULL   0xFFFFFFFFFFFFFFFFL

#define INDEX_DATA_NULL_STR   "NULL"
#define INDEX_DATA_NULL_STR_L "null"

char JSON_COLUMN[] = "JSON";
char JSON_VALUE_DELIM = '&';

char* idxInt2str(int64_t val, char* dst, int radix) {
  char     buffer[65] = {0};
  char*    p;
  int64_t  new_val;
  uint64_t uval = (uint64_t)val;

  if (radix < 0) {
    if (val < 0) {
      *dst++ = '-';
      uval = (uint64_t)0 - uval; /* Avoid integer overflow in (-val) for LLONG_MIN (BUG#31799). */
    }
  }
  p = &buffer[sizeof(buffer) - 1];
  *p = '\0';
  new_val = (int64_t)(uval / 10);
  *--p = '0' + (char)(uval - (uint64_t)new_val * 10);
  val = new_val;

  while (val != 0) {
    new_val = val / 10;
    *--p = '0' + (char)(val - new_val * 10);
    val = new_val;
  }
  while ((*dst++ = *p++) != 0)
    ;
  return dst - 1;
}
__compar_fn_t idxGetCompar(int8_t type) {
  if (type == TSDB_DATA_TYPE_BINARY || type == TSDB_DATA_TYPE_VARBINARY || type == TSDB_DATA_TYPE_NCHAR ||
      type == TSDB_DATA_TYPE_GEOMETRY) {
    return (__compar_fn_t)strcmp;
  }
  return getComparFunc(type, 0);
}
static FORCE_INLINE TExeCond tCompareLessThan(void* a, void* b, int8_t type) {
  __compar_fn_t func = idxGetCompar(type);
  if (func == NULL) {
    return BREAK;
  }
  return tCompare(func, QUERY_LESS_THAN, a, b, type);
}
static FORCE_INLINE TExeCond tCompareLessEqual(void* a, void* b, int8_t type) {
  __compar_fn_t func = idxGetCompar(type);
  if (func == NULL) {
    return BREAK;
  }
  return tCompare(func, QUERY_LESS_EQUAL, a, b, type);
}
static FORCE_INLINE TExeCond tCompareGreaterThan(void* a, void* b, int8_t type) {
  __compar_fn_t func = idxGetCompar(type);
  if (func == NULL) {
    return BREAK;
  }
  return tCompare(func, QUERY_GREATER_THAN, a, b, type);
}
static FORCE_INLINE TExeCond tCompareGreaterEqual(void* a, void* b, int8_t type) {
  __compar_fn_t func = idxGetCompar(type);
  if (func == NULL) {
    return BREAK;
  }
  return tCompare(func, QUERY_GREATER_EQUAL, a, b, type);
}

static FORCE_INLINE TExeCond tCompareContains(void* a, void* b, int8_t type) {
  __compar_fn_t func = idxGetCompar(type);
  if (func == NULL) {
    return BREAK;
  }
  return tCompare(func, QUERY_TERM, a, b, type);
}

#define CHECKCOMERROR(expr) \
  do {                      \
    if ((expr) != 0) {      \
      return FAILED;        \
    }                       \
  } while (0)

static FORCE_INLINE TExeCond tCompareEqual(void* a, void* b, int8_t type) {
  __compar_fn_t func = idxGetCompar(type);
  if (func == NULL) {
    return BREAK;
  }
  return tCompare(func, QUERY_TERM, a, b, type);
}
TExeCond tCompare(__compar_fn_t func, int8_t cmptype, void* a, void* b, int8_t dtype) {
  if (dtype == TSDB_DATA_TYPE_BINARY || dtype == TSDB_DATA_TYPE_NCHAR || dtype == TSDB_DATA_TYPE_VARBINARY ||
      dtype == TSDB_DATA_TYPE_GEOMETRY) {
    return tDoCompare(func, cmptype, a, b);
  }
#if 1
  if (dtype == TSDB_DATA_TYPE_TIMESTAMP) {
    int64_t va;
    CHECKCOMERROR(taosStr2int64(a, &va));
    int64_t vb;
    CHECKCOMERROR(taosStr2int64(b, &vb));
    return tDoCompare(func, cmptype, &va, &vb);
  } else if (dtype == TSDB_DATA_TYPE_BOOL || dtype == TSDB_DATA_TYPE_UTINYINT) {
    uint8_t va;
    CHECKCOMERROR(taosStr2int8(a, &va));
    uint8_t vb;
    CHECKCOMERROR(taosStr2int8(b, &vb));
    return tDoCompare(func, cmptype, &va, &vb);
  } else if (dtype == TSDB_DATA_TYPE_TINYINT) {
    int8_t va;
    CHECKCOMERROR(taosStr2int8(a, &va));
    int8_t vb;
    CHECKCOMERROR(taosStr2int8(b, &vb));
    return tDoCompare(func, cmptype, &va, &vb);
  } else if (dtype == TSDB_DATA_TYPE_SMALLINT) {
    int16_t va;
    CHECKCOMERROR(taosStr2int16(a, &va));
    int16_t vb;
    CHECKCOMERROR(taosStr2int16(b, &vb));
    return tDoCompare(func, cmptype, &va, &vb);
  } else if (dtype == TSDB_DATA_TYPE_USMALLINT) {
    uint16_t va;
    CHECKCOMERROR(taosStr2int16(a, &va));
    uint16_t vb;
    CHECKCOMERROR(taosStr2int16(b, &vb));
    return tDoCompare(func, cmptype, &va, &vb);
  } else if (dtype == TSDB_DATA_TYPE_INT) {
    int32_t va;
    CHECKCOMERROR(taosStr2int32(a, &va));
    int32_t vb;
    CHECKCOMERROR(taosStr2int32(b, &vb));
    return tDoCompare(func, cmptype, &va, &vb);
  } else if (dtype == TSDB_DATA_TYPE_UINT) {
    uint32_t va;
    CHECKCOMERROR(taosStr2int32(a, &va));
    uint32_t vb;
    CHECKCOMERROR(taosStr2int32(b, &vb));
    return tDoCompare(func, cmptype, &va, &vb);
  } else if (dtype == TSDB_DATA_TYPE_BIGINT) {
    int64_t va;
    CHECKCOMERROR(taosStr2int64(a, &va));
    int64_t vb;
    CHECKCOMERROR(taosStr2int64(b, &vb));
    return tDoCompare(func, cmptype, &va, &vb);
  } else if (dtype == TSDB_DATA_TYPE_UBIGINT) {
    uint64_t va, vb;
    if (0 != toUInteger(a, strlen(a), 10, &va) || 0 != toUInteger(b, strlen(b), 10, &vb)) {
      return CONTINUE;
    }
    return tDoCompare(func, cmptype, &va, &vb);
  } else if (dtype == TSDB_DATA_TYPE_FLOAT) {
    float va = taosStr2Float(a, NULL);
    if (ERRNO == ERANGE && va == -1) {
      return CONTINUE;
    }
    float vb = taosStr2Float(b, NULL);
    if (ERRNO == ERANGE && va == -1) {
      return CONTINUE;
    }
    return tDoCompare(func, cmptype, &va, &vb);
  } else if (dtype == TSDB_DATA_TYPE_DOUBLE) {
    double va = taosStr2Double(a, NULL);
    if (ERRNO == ERANGE && va == -1) {
      return CONTINUE;
    }
    double vb = taosStr2Double(b, NULL);
    if (ERRNO == ERANGE && va == -1) {
      return CONTINUE;
    }
    return tDoCompare(func, cmptype, &va, &vb);
  }
  return BREAK;
#endif
}
TExeCond tDoCompare(__compar_fn_t func, int8_t comparType, void* a, void* b) {
  // optime later
  int32_t ret = func(a, b);
  switch (comparType) {
    case QUERY_LESS_THAN:
      if (ret < 0) return MATCH;
      break;
    case QUERY_LESS_EQUAL: {
      if (ret <= 0) return MATCH;
      break;
    }
    case QUERY_GREATER_THAN: {
      if (ret > 0) return MATCH;
      break;
    }
    case QUERY_GREATER_EQUAL: {
      if (ret >= 0) return MATCH;
      break;
    }
    case QUERY_TERM: {
      if (ret == 0) return MATCH;
      break;
    }
    default:
      return BREAK;
  }
  return CONTINUE;
}

static TExeCond (*rangeCompare[])(void* a, void* b, int8_t type) = {
    tCompareLessThan, tCompareLessEqual, tCompareGreaterThan, tCompareGreaterEqual, tCompareContains, tCompareEqual};

_cache_range_compare idxGetCompare(RangeType ty) { return rangeCompare[ty]; }

char* idxPackJsonData(SIndexTerm* itm) {
  /*
   * |<-----colname---->|<-----dataType---->|<--------colVal---------->|
   * |<-----string----->|<-----uint8_t----->|<----depend on dataType-->|
   */
  uint8_t ty = IDX_TYPE_GET_TYPE(itm->colType);

  int32_t sz = itm->nColName + itm->nColVal + sizeof(uint8_t) + sizeof(JSON_VALUE_DELIM) * 2 + 1;
  char*   buf = (char*)taosMemoryCalloc(1, sz);
  if (buf == NULL) {
    return NULL;
  }
  char* p = buf;

  memcpy(p, itm->colName, itm->nColName);
  p += itm->nColName;

  memcpy(p, &JSON_VALUE_DELIM, sizeof(JSON_VALUE_DELIM));
  p += sizeof(JSON_VALUE_DELIM);

  memcpy(p, &ty, sizeof(ty));
  p += sizeof(ty);

  memcpy(p, &JSON_VALUE_DELIM, sizeof(JSON_VALUE_DELIM));
  p += sizeof(JSON_VALUE_DELIM);

  memcpy(p, itm->colVal, itm->nColVal);

  return buf;
}

char* idxPackJsonDataPrefix(SIndexTerm* itm, int32_t* skip) {
  /*
   * |<-----colname---->|<-----dataType---->|<--------colVal---------->|
   * |<-----string----->|<-----uint8_t----->|<----depend on dataType-->|
   */
  uint8_t ty = IDX_TYPE_GET_TYPE(itm->colType);

  int32_t sz = itm->nColName + itm->nColVal + sizeof(uint8_t) + sizeof(JSON_VALUE_DELIM) * 2 + 1;
  char*   buf = (char*)taosMemoryCalloc(1, sz);
  if (buf == NULL) {
    return NULL;
  }
  char* p = buf;

  memcpy(p, itm->colName, itm->nColName);
  p += itm->nColName;

  memcpy(p, &JSON_VALUE_DELIM, sizeof(JSON_VALUE_DELIM));
  p += sizeof(JSON_VALUE_DELIM);

  memcpy(p, &ty, sizeof(ty));
  p += sizeof(ty);

  memcpy(p, &JSON_VALUE_DELIM, sizeof(JSON_VALUE_DELIM));
  p += sizeof(JSON_VALUE_DELIM);

  *skip = p - buf;

  return buf;
}
char* idxPackJsonDataPrefixNoType(SIndexTerm* itm, int32_t* skip) {
  /*
   * |<-----colname---->|<-----dataType---->|<--------colVal---------->|
   * |<-----string----->|<-----uint8_t----->|<----depend on dataType-->|
   */
  uint8_t ty = IDX_TYPE_GET_TYPE(itm->colType);

  int32_t sz = itm->nColName + itm->nColVal + sizeof(uint8_t) + sizeof(JSON_VALUE_DELIM) * 2 + 1;
  char*   buf = (char*)taosMemoryCalloc(1, sz);
  if (buf == NULL) {
    return NULL;
  }

  char* p = buf;

  memcpy(p, itm->colName, itm->nColName);
  p += itm->nColName;

  memcpy(p, &JSON_VALUE_DELIM, sizeof(JSON_VALUE_DELIM));
  p += sizeof(JSON_VALUE_DELIM);
  *skip = p - buf;

  return buf;
}

int idxUidCompare(const void* a, const void* b) {
  uint64_t l = *(uint64_t*)a;
  uint64_t r = *(uint64_t*)b;
  return l - r;
}

int32_t idxConvertDataToStr(void* src, int8_t type, void** dst) {
  if (src == NULL) {
    *dst = taosStrndup(INDEX_DATA_NULL_STR, (int)strlen(INDEX_DATA_NULL_STR));
    if (*dst == NULL) {
      return terrno;
    }
    return (int32_t)strlen(INDEX_DATA_NULL_STR);
  }
  int     tlen = tDataTypes[type].bytes;
  int32_t bufSize = 64;
  switch (type) {
    case TSDB_DATA_TYPE_TIMESTAMP:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      TAOS_UNUSED(idxInt2str(*(int64_t*)src, *dst, -1));
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_BOOL:
    case TSDB_DATA_TYPE_UTINYINT:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      TAOS_UNUSED(idxInt2str(*(uint8_t*)src, *dst, 1));
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_TINYINT:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      TAOS_UNUSED(idxInt2str(*(int8_t*)src, *dst, 1));
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_SMALLINT:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      TAOS_UNUSED(idxInt2str(*(int16_t*)src, *dst, -1));
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_USMALLINT:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      TAOS_UNUSED(idxInt2str(*(uint16_t*)src, *dst, -1));
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_INT:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      TAOS_UNUSED(idxInt2str(*(int32_t*)src, *dst, -1));
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_UINT:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      TAOS_UNUSED(idxInt2str(*(uint32_t*)src, *dst, 1));
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_BIGINT:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      sprintf(*dst, "%" PRIu64, *(uint64_t*)src);
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_UBIGINT:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      TAOS_UNUSED(idxInt2str(*(uint64_t*)src, *dst, 1));
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_FLOAT:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      sprintf(*dst, "%.9lf", *(float*)src);
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_DOUBLE:
      *dst = taosMemoryCalloc(1, bufSize + 1);
      if (*dst == NULL) {
        return terrno;
      }
      sprintf(*dst, "%.9lf", *(double*)src);
      tlen = strlen(*dst);
      break;
    case TSDB_DATA_TYPE_NCHAR: {
      tlen = taosEncodeBinary(NULL, varDataVal(src), varDataLen(src));
      *dst = taosMemoryCalloc(1, tlen + 1);
      if (*dst == NULL) {
        return terrno;
      }
      tlen = taosEncodeBinary(dst, varDataVal(src), varDataLen(src));
      *dst = (char*)*dst - tlen;
      break;
    }
    case TSDB_DATA_TYPE_VARCHAR:  // TSDB_DATA_TYPE_BINARY
    case TSDB_DATA_TYPE_VARBINARY:
    case TSDB_DATA_TYPE_GEOMETRY: {
      tlen = taosEncodeBinary(NULL, varDataVal(src), varDataLen(src));
      *dst = taosMemoryCalloc(1, tlen + 1);
      if (*dst == NULL) {
        return terrno;
      }
      tlen = taosEncodeBinary(dst, varDataVal(src), varDataLen(src));
      *dst = (char*)*dst - tlen;
      break;
    }
    default:
      tlen = TSDB_CODE_INVALID_DATA_FMT;
      break;
  }
  return tlen;
}
