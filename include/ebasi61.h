#ifndef _EBASI61_H
#define _EBASI61_H

#include <stdint.h>
#include <stdlib.h>

/* ebasi61 is encoding of numbers and strings using symbols 0-9, a-z and A-Y. The letter Z is purposefully
 * reserved for applications to use it as separator. So in order to encode 5 numbers as single string just
 * join their string ebasi61 representation with 'Z' char between them. Then to decode split by 'Z' and 
 * decode the numbers.
 * 
 * This library only deals with encoding and decoding. 
 * 
 * Numbers are stored with least significant % 61 first. This eases the encoding, you can directly encode in
 * user's provided buffer. It does not complicate decoding.
 * 
 * Empty string is valid representation of 0.
 * 
 * Library needs init function as it will prepare fast lookup array for decoding two bytes at a time. Memory
 * requirement is around 200K.
 * 
 * Functions do not produce nul terminated strings and always return the length as the last size_t * parameter.
 * NULL is not acceptable there.
 * 
 * Binary/hex data encoding is different. Data is split in 8 bytes chunks, compressed as number and padded with
 * zero in the end to 11 bytes. If there is remainder of data, it is split in chunks if 4,2 and finally 1 bytes.
 * 
 * Binary data length needs to be even, hex data length needs to divide by 4. Otherwise during decoding zero byte
 * or zeroes (up to 3) in case of hex will be appended. So if you pass real binary data like strings, passing it's
 * length may be necessary unless trailing zeroes are ok. Still in this case strlen() needs to be called as len.
 * Also during encoding of odd len data access of one byte past provided length will be preformed.
 * 
 * The protocol is mostly designed for passing multiple numbers and their signatures in URLs and domain names.
 * Multiple numbers may decode to one value (one will be overflow number), same is valid for binary data.
 */

#define EBASI61_INT64MAXLEN 11
#define EBASI61_INT32MAXLEN 6
#define EBASI61_INT16MAXLEN 3
#define EBASI61_INT8MAXLEN 2

#define EBASI61_BINARYLEN(x) (((x)/8)*11+(((x)%8)/4)*6+(((x)%4)/2)*3+((x)%2)*2)

int ebasi61_init();
void ebasi61_encode_uint64(uint64_t n, char *dest, size_t *len);
void ebasi61_encode_uint32(uint32_t n, char *dest, size_t *len);
void ebasi61_encode_uint16(uint16_t n, char *dest, size_t *len);
void ebasi61_encode_uint8(uint8_t n, char *dest, size_t *len);

void ebasi61_encode_binary(const void *data, size_t datalen, char *dest, size_t *len);
void ebasi61_encode_hex(const char *data, size_t datalen, char *dest, size_t *len);

uint64_t ebasi61_decode_uint64(const char *str, size_t len);
uint32_t ebasi61_decode_uint32(const char *str, size_t len);
uint16_t ebasi61_decode_uint16(const char *str, size_t len);
uint8_t ebasi61_decode_uint8(const char *str, size_t len);

void ebasi61_decode_binary(const char *data, size_t datalen, void *dest, size_t *len);
void ebasi61_decode_hex(const char *data, size_t datalen, char *dest, size_t *len);

#endif
