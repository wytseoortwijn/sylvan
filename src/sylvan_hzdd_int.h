/*
 * Copyright 2016 Tom van Dijk, Johannes Kepler University Linz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Internals for hybrid multi-terminal ZDD/BDDs
 *
 * This approach puts tag on edges in the 16-byte nodes.
 * Limitations:
 * - only 32 bits to address nodes (so max 2^32 = 4 billion nodes = 96 GB memory)
 * - only 20 bits for variables (so max 2^20 = 1,048,576 variables)
 *
 * Bits (not as they are actually in memory)
 * 127        1 comp            complement on the high edge
 * 126        1 leaf            set if this is a MT leaf node
 * 125        1 mark            used for node marking
 * 124        1 map             is a MAP node, for compose etc
 * 123..104  20 high tag        tag of the high edge
 * 103..72   32 high index      index of the high edge
 *  71..52   20 variable        variable of this node
 *  51..32   20 low tag         tag of the low edge
 *  31..0    32 low index       index of the low edge
 *
 * Lil endian: HHHH HHHH TTTT VTVV | VVLF LLLL LLTL TTTT
 * Big endian: VVVT TTTT HHHH HHHH | TTTT TLLL LLLL LFVV
 * Reading uint32_t +6:       LFVV VVVT
 *
 * Leaf nodes: VVVV VVVV VVVV VVVV | TTTT TTTT xxxx xFxx
 *
 * Edges:      CxxT TTTT IIII IIII          so only 11 bits for operation id!
 *                                          this limits operation cache...
 *                                          edges require 53 bits (instead of 41)
 */

#ifndef SYLVAN_HZDD_INT_H
#define SYLVAN_HZDD_INT_H

/**
 * HZDD node structure
 */
typedef struct __attribute__((packed)) hzddnode {
    uint64_t a, b;
} * hzddnode_t; // 16 bytes

/**
 * Macros to work with the HZDD type and the complement marks
 */
#define HZDD_GETINDEX(hzdd)          ((hzdd & 0x00000000ffffffff))
#define HZDD_GETNODE(hzdd)           ((hzddnode_t)llmsset_index_to_ptr(nodes, HZDD_GETINDEX(hzdd)))
#define HZDD_GETTAG(hzdd)            ((hzdd & 0x000fffff00000000)>>32)
#define HZDD_SETTAG(hzdd, tag)       ((hzdd & 0xfff00000ffffffff) | ((uint64_t)tag << 32))
#define HZDD_NOTAG(hzdd)             ((hzdd | 0x000fffff00000000))
#define HZDD_HASMARK(s)              (s&hzdd_complement?1:0)
#define HZDD_TOGGLEMARK(s)           (s^hzdd_complement)
#define HZDD_STRIPMARK(s)            (s&~hzdd_complement)
#define HZDD_TRANSFERMARK(from, to)  (to ^ (from & hzdd_complement))
// Equal under mark
#define HZDD_EQUALM(a, b)            ((((a)^(b))&(~hzdd_complement))==0)

static inline int __attribute__((unused))
hzddnode_isleaf(hzddnode_t n)
{
    return n->b & 0x0000000000000400 ? 1 : 0;
}

static inline uint32_t __attribute__((unused))
hzddnode_gettype(hzddnode_t n)
{
    return (uint32_t)(n->b >> 32);
}

static inline uint64_t __attribute__((unused))
hzddnode_getvalue(hzddnode_t n)
{
    return n->a;
}

static inline int __attribute__((unused))
hzddnode_getcomp(hzddnode_t n)
{
    return n->b & 0x0000000000000800 ? 1 : 0;
}

static inline uint64_t __attribute__((unused))
hzddnode_getlow(hzddnode_t n)
{
    return (n->b & 0xfffffffffffff000) >> 12;
}

static inline uint64_t __attribute__((unused))
hzddnode_gethigh(hzddnode_t n)
{
    return (n->a & 0x000fffffffffffff) | (hzddnode_getcomp(n) ? 0x8000000000000000 : 0);
}

static inline uint32_t __attribute__((unused))
hzddnode_getvariable(hzddnode_t n)
{
    uint32_t *ptr = (uint32_t*)((uint8_t*)n + 6);
    return (*ptr & 0x00fffff0) >> 4;
}

static inline int __attribute__((unused))
hzddnode_getmark(hzddnode_t n)
{
    return n->b & 0x0000000000000200 ? 1 : 0;
}

static inline void __attribute__((unused))
hzddnode_setmark(hzddnode_t n, int mark)
{
    if (mark) n->b |= 0x0000000000000200;
    else n->b &= 0xfffffffffffffdff;
}

static inline void __attribute__((unused))
hzddnode_makeleaf(hzddnode_t n, uint32_t type, uint64_t value)
{
    n->a = value;
    n->b = 0x0000000000000400 | ((uint64_t)type << 32);
}

static inline void __attribute__((unused))
hzddnode_makenode(hzddnode_t n, uint32_t var, uint64_t low, uint64_t high)
{
    n->a = high & 0x000fffffffffffff;
    n->b = low << 12 | (high & 0x8000000000000000 ? 0x0800 : 0);
    *(uint32_t*)((uint8_t*)n + 6) |= var<<4;
}

static inline void __attribute__((unused))
hzddnode_makemapnode(hzddnode_t n, uint32_t var, uint64_t low, uint64_t high)
{
    n->a = high & 0x000fffffffffffff;
    n->b = low << 12 | (high & 0x8000000000000000 ? 0x0900 : 0x0100);
    *(uint32_t*)((uint8_t*)n + 6) |= var<<4;
}

static inline int __attribute__((unused))
hzddnode_ismapnode(hzddnode_t n)
{
    return n->b & 0x0000000000000100 ? 1 : 0;
}

static HZDD __attribute__((unused))
hzddnode_low(HZDD hzdd, hzddnode_t node)
{
    return HZDD_TRANSFERMARK(hzdd, hzddnode_getlow(node));
}

static HZDD __attribute__((unused))
hzddnode_high(HZDD hzdd, hzddnode_t node)
{
    return hzddnode_gethigh(node);
    (void)hzdd;
}

#endif
