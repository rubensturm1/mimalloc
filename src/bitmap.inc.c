#pragma once
#ifndef MI_BITMAP_H
#define MI_BITMAP_H

#include "mimalloc.h"
#include "mimalloc-internal.h"

// Use bit scan forward to quickly find the first zero bit if it is available
#if defined(_MSC_VER)
#define MI_HAVE_BITSCAN
#include <intrin.h>
static inline size_t mi_bsf(uintptr_t x) {
  if (x==0) return 8*MI_INTPTR_SIZE;
  DWORD idx;
  MI_64(_BitScanForward)(&idx, x);
  return idx;
}
static inline size_t mi_bsr(uintptr_t x) {
  if (x==0) return 8*MI_INTPTR_SIZE;
  DWORD idx;
  MI_64(_BitScanReverse)(&idx, x);
  return idx;
}
#elif defined(__GNUC__) || defined(__clang__)
#define MI_HAVE_BITSCAN
#if (INTPTR_MAX == LONG_MAX)
# define MI_L(x)  x##l
#else
# define MI_L(x)  x##ll
#endif
static inline size_t mi_bsf(uintptr_t x) {
  return (x==0 ? 8*MI_INTPTR_SIZE : MI_L(__builtin_ctz)(x));
}
static inline size_t mi_bsr(uintptr_t x) {
  return (x==0 ? 8*MI_INTPTR_SIZE : (8*MI_INTPTR_SIZE - 1) - MI_L(__builtin_clz)(x));
}
#endif


#define MI_BITMAP_FIELD_BITS   (8*MI_INTPTR_SIZE)
#define MI_BITMAP_FIELD_FULL   (~((uintptr_t)0))   // all bits set

// An atomic bitmap of `uintptr_t` fields
typedef volatile _Atomic(uintptr_t)  mi_bitmap_field_t;
typedef mi_bitmap_field_t*           mi_bitmap_t;

// A bitmap index is the index of the bit in a bitmap.
typedef size_t mi_bitmap_index_t;

// Create a bit index.
static inline mi_bitmap_index_t mi_bitmap_index_create(size_t idx, size_t bitidx) {
  mi_assert_internal(bitidx < MI_BITMAP_FIELD_BITS);
  return (idx*MI_BITMAP_FIELD_BITS) + bitidx;
}

// Get the field index from a bit index.
static inline size_t mi_bitmap_index_field(mi_bitmap_index_t bitmap_idx) {
  return (bitmap_idx / MI_BITMAP_FIELD_BITS);
}

// Get the bit index in a bitmap field
static inline size_t mi_bitmap_index_bit_in_field(mi_bitmap_index_t bitmap_idx) {
  return (bitmap_idx % MI_BITMAP_FIELD_BITS);
}

// The bit mask for a given number of blocks at a specified bit index.
static uintptr_t mi_bitmap_mask_(size_t count, size_t bitidx) {
  mi_assert_internal(count + bitidx <= MI_BITMAP_FIELD_BITS);
  return ((((uintptr_t)1 << count) - 1) << bitidx);
}

// Try to atomically claim a sequence of `count` bits in a single field at `idx` in `bitmap`.
// Returns `true` on success.
static inline bool mi_bitmap_try_claim_field(mi_bitmap_t bitmap, size_t idx, const size_t count, mi_bitmap_index_t* bitmap_idx) 
{  
  mi_assert_internal(bitmap_idx != NULL);
  volatile _Atomic(uintptr_t)* field = &bitmap[idx];
  uintptr_t map  = mi_atomic_read(field);
  if (map==MI_BITMAP_FIELD_FULL) return false; // short cut

  // search for 0-bit sequence of length count
  const uintptr_t mask = mi_bitmap_mask_(count, 0);
  const size_t    bitidx_max = MI_BITMAP_FIELD_BITS - count;

#ifdef MI_HAVE_BITSCAN
  size_t bitidx = mi_bsf(~map);    // quickly find the first zero bit if possible
#else
  size_t bitidx = 0;               // otherwise start at 0
#endif
  uintptr_t m = (mask << bitidx);     // invariant: m == mask shifted by bitidx

  // scan linearly for a free range of zero bits
  while (bitidx <= bitidx_max) {
    if ((map & m) == 0) {  // are the mask bits free at bitidx?
      mi_assert_internal((m >> bitidx) == mask); // no overflow?
      uintptr_t newmap = map | m;
      mi_assert_internal((newmap^map) >> bitidx == mask);
      if (!mi_atomic_cas_weak(field, newmap, map)) {  // TODO: use strong cas here?
        // no success, another thread claimed concurrently.. keep going
        map = mi_atomic_read(field);
        continue;
      }
      else {
        // success, we claimed the bits!        
        *bitmap_idx = mi_bitmap_index_create(idx, bitidx);
        return true;
      }
    }
    else {
      // on to the next bit range
#ifdef MI_HAVE_BITSCAN
      size_t shift = (count == 1 ? 1 : mi_bsr(map & m) - bitidx + 1);
      mi_assert_internal(shift > 0 && shift <= count);
#else
      size_t shift = 1;
#endif
      bitidx += shift;
      m <<= shift;
    }
  }
  // no bits found
  return false;
}


// Find `count` bits of 0 and set them to 1 atomically; returns `true` on success.
// For now, `count` can be at most MI_BITMAP_FIELD_BITS and will never span fields.
static inline bool mi_bitmap_try_claim(mi_bitmap_t bitmap, size_t bitmap_fields, size_t count, mi_bitmap_index_t* bitmap_idx) {
  for (size_t idx = 0; idx < bitmap_fields; idx++) {
    if (mi_bitmap_try_claim_field(bitmap, idx, count, bitmap_idx)) {
      return true;
    }
  }
  return false;
}

// Set `count` bits at `bitmap_idx` to 0 atomically
static inline void mi_bitmap_unclaim(mi_bitmap_t bitmap, size_t bitmap_fields, size_t count, mi_bitmap_index_t bitmap_idx) {
  const size_t idx = mi_bitmap_index_field(bitmap_idx);
  const size_t bitidx = mi_bitmap_index_bit_in_field(bitmap_idx);
  const uintptr_t mask = mi_bitmap_mask_(count, bitidx);
  mi_assert_internal(bitmap_fields > idx); UNUSED(bitmap_fields);
  mi_assert_internal((bitmap[idx] & mask) == mask);
  mi_atomic_and(&bitmap[idx], ~mask);
}


// Set `count` bits at `bitmap_idx` to 1 atomically
// Returns `true` if all `count` bits were 0 previously
static inline bool mi_bitmap_claim(mi_bitmap_t bitmap, size_t bitmap_fields, size_t count, mi_bitmap_index_t bitmap_idx) {
  const size_t idx = mi_bitmap_index_field(bitmap_idx);
  const size_t bitidx = mi_bitmap_index_bit_in_field(bitmap_idx);
  const uintptr_t mask = mi_bitmap_mask_(count, bitidx);
  mi_assert_internal(bitmap_fields > idx); UNUSED(bitmap_fields);
  // mi_assert_internal((bitmap[idx] & mask) == 0);
  uintptr_t prev = mi_atomic_or(&bitmap[idx], mask);
  return ((prev & mask) == 0);
}

#endif