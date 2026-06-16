//===- CryptoUtils.h - Cryptographically Secure Pseudo-Random Generator ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains includes and defines for the AES CTR PRNG
// The AES implementation has been derived and adapted
// from libtomcrypt (see http://libtom.org)
// Created on: 22 juin 2012
// Author(s): jrinaldini, pjunod
//===----------------------------------------------------------------------===//
#ifndef _OBFUSCATION_CRYPTUTILS_H
#define _OBFUSCATION_CRYPTUTILS_H

#include "llvm/Support/ManagedStatic.h"

#include <cstdint>
#include <string>

namespace llvm {

class CryptoUtils;
extern ManagedStatic<CryptoUtils> Cryptoutils;

#define BYTE(x, n) (((x) >> (8 * (n))) & 0xFF)

#if defined(__i386) || defined(__i386__) || defined(_M_IX86) ||                \
    defined(INTEL_CC) || defined(_WIN64) || defined(_WIN32)

#ifndef ENDIAN_LITTLE
#define ENDIAN_LITTLE
#endif
#define ENDIAN_32BITWORD

#if !defined(_WIN64) || !defined(_WIN32)
#ifndef UNALIGNED
#define UNALIGNED
#endif
#endif

#elif defined(__alpha)

#ifndef ENDIAN_LITTLE
#define ENDIAN_LITTLE
#endif
#define ENDIAN_64BITWORD

#elif defined(__x86_64__)

#ifndef ENDIAN_LITTLE
#define ENDIAN_LITTLE
#endif
#define ENDIAN_64BITWORD
#define UNALIGNED

#elif (defined(__R5900) || defined(R5900) || defined(__R5900__)) &&            \
    (defined(_mips) || defined(__mips__) || defined(mips))

#ifndef ENDIAN_LITTLE
#define ENDIAN_LITTLE
#endif
#define ENDIAN_64BITWORD

#elif defined(__sparc) || defined(__aarch64__)

#ifndef ENDIAN_BIG
#define ENDIAN_BIG
#endif
#if defined(__arch64__) || defined(__aarch64__)
#define ENDIAN_64BITWORD
#else
#define ENDIAN_32BITWORD
#endif

#endif

#if defined(__BIG_ENDIAN__) || defined(_BIG_ENDIAN)
#define ENDIAN_BIG
#endif

#if !defined(ENDIAN_BIG) && !defined(ENDIAN_LITTLE)
#error                                                                         \
    "Unknown endianness of the compilation platform, check this header aes_encrypt.h"
#endif

#define CryptoUtils_POOL_SIZE (0x1 << 17) // 2^17

class CryptoUtils {
public:
  CryptoUtils();
  ~CryptoUtils();

  char *getSeed();
  void getBytes(char *Buffer, const int Len);
  char getChar();
  bool prngSeed(std::string const &Seed);

  // Returns a uniformly distributed 8-bit value
  uint8_t getUint8T();
  // Returns a uniformly distributed 16-bit value
  uint16_t getUint16T();
  // Returns a uniformly distributed 32-bit value
  uint32_t getUint32T();
  // Returns an integer uniformly distributed on [0, max[
  uint32_t getRange(const uint32_t Max);
  // Returns a uniformly distributed 64-bit value
  uint64_t getUint64T();

  // Scramble a 32-bit value depending on a 128-bit value
  unsigned scramble32(const unsigned In, const char Key[16]);

  int sha256(const char *Msg, unsigned char *Hash);

private:
  uint32_t Ks[44];
  char Key[16];
  char Ctr[16];
  char Pool[CryptoUtils_POOL_SIZE];
  uint32_t Idx;
  std::string Seed;
  bool Seeded;

  typedef struct {
    uint64_t Length;
    uint32_t State[8], Curlen;
    unsigned char Buf[64];
  } sha256_state;

  void aesComputeKs(uint32_t *Ks, const char *K);
  void aesEncrypt(char *Out, const char *In, const uint32_t *Ks);
  bool prngSeed();
  void incCtr();
  void populatePool();
  int sha256Done(sha256_state *Md, unsigned char *Out);
  int sha256Init(sha256_state *Md);
  static int sha256Compress(sha256_state *Md, const unsigned char *Buf);
  int sha256Process(sha256_state *Md, const unsigned char *In,
                    unsigned long Inlen);
};
} // namespace llvm

#endif
