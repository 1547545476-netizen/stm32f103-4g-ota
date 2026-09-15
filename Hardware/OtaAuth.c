#include "OtaAuth.h"
#include "W25Q64.h"
#include <string.h>

/*
 * 本文件实现SHA-256和HMAC-SHA256，不依赖动态内存。
 * CRC32只能发现随机损坏，攻击者可以修改固件后重新计算CRC32；HMAC还混入只有
 * 发布端和设备知道的密钥，因此不知道密钥的人无法为恶意固件生成正确标签。
 * 当前保存前16字节标签，提供128位认证强度，同时节省AT24C02空间。
 */

#define SHA256_BLOCK_SIZE       64U
#define SHA256_DIGEST_SIZE      32U
#define OTA_AUTH_READ_SIZE      256U

typedef struct {
	uint32_t state[8];
	uint64_t totalBytes;
	uint8_t block[SHA256_BLOCK_SIZE];
	uint32_t blockLength;
} Sha256Context_t;

typedef struct {
	Sha256Context_t inner;
	uint8_t outerPad[SHA256_BLOCK_SIZE];
} HmacSha256Context_t;

static uint8_t OtaAuth_ReadBuffer[OTA_AUTH_READ_SIZE];

static const uint32_t Sha256K[64] = {
	0x428A2F98UL, 0x71374491UL, 0xB5C0FBCFUL, 0xE9B5DBA5UL,
	0x3956C25BUL, 0x59F111F1UL, 0x923F82A4UL, 0xAB1C5ED5UL,
	0xD807AA98UL, 0x12835B01UL, 0x243185BEUL, 0x550C7DC3UL,
	0x72BE5D74UL, 0x80DEB1FEUL, 0x9BDC06A7UL, 0xC19BF174UL,
	0xE49B69C1UL, 0xEFBE4786UL, 0x0FC19DC6UL, 0x240CA1CCUL,
	0x2DE92C6FUL, 0x4A7484AAUL, 0x5CB0A9DCUL, 0x76F988DAUL,
	0x983E5152UL, 0xA831C66DUL, 0xB00327C8UL, 0xBF597FC7UL,
	0xC6E00BF3UL, 0xD5A79147UL, 0x06CA6351UL, 0x14292967UL,
	0x27B70A85UL, 0x2E1B2138UL, 0x4D2C6DFCUL, 0x53380D13UL,
	0x650A7354UL, 0x766A0ABBUL, 0x81C2C92EUL, 0x92722C85UL,
	0xA2BFE8A1UL, 0xA81A664BUL, 0xC24B8B70UL, 0xC76C51A3UL,
	0xD192E819UL, 0xD6990624UL, 0xF40E3585UL, 0x106AA070UL,
	0x19A4C116UL, 0x1E376C08UL, 0x2748774CUL, 0x34B0BCB5UL,
	0x391C0CB3UL, 0x4ED8AA4AUL, 0x5B9CCA4FUL, 0x682E6FF3UL,
	0x748F82EEUL, 0x78A5636FUL, 0x84C87814UL, 0x8CC70208UL,
	0x90BEFFFAUL, 0xA4506CEBUL, 0xBEF9A3F7UL, 0xC67178F2UL
};

static uint32_t Sha256_RotateRight(uint32_t value, uint8_t bits)
{
	return (value >> bits) | (value << (32U - bits));
}

static uint32_t Sha256_ReadBigEndian32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) |
		((uint32_t)data[1] << 16) |
		((uint32_t)data[2] << 8) |
		(uint32_t)data[3];
}

static void Sha256_WriteBigEndian32(uint8_t *data, uint32_t value)
{
	data[0] = (uint8_t)(value >> 24);
	data[1] = (uint8_t)(value >> 16);
	data[2] = (uint8_t)(value >> 8);
	data[3] = (uint8_t)value;
}

static void Sha256_Transform(Sha256Context_t *ctx, const uint8_t block[SHA256_BLOCK_SIZE])
{
	uint32_t w[64];
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t e;
	uint32_t f;
	uint32_t g;
	uint32_t h;
	uint32_t s0;
	uint32_t s1;
	uint32_t choice;
	uint32_t majority;
	uint32_t temp1;
	uint32_t temp2;
	uint32_t i;

	for (i = 0U; i < 16U; i++)
	{
		w[i] = Sha256_ReadBigEndian32(&block[i * 4U]);
	}
	for (i = 16U; i < 64U; i++)
	{
		s0 = Sha256_RotateRight(w[i - 15U], 7U) ^
			Sha256_RotateRight(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
		s1 = Sha256_RotateRight(w[i - 2U], 17U) ^
			Sha256_RotateRight(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
		w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0U; i < 64U; i++)
	{
		s1 = Sha256_RotateRight(e, 6U) ^ Sha256_RotateRight(e, 11U) ^
			Sha256_RotateRight(e, 25U);
		choice = (e & f) ^ ((~e) & g);
		temp1 = h + s1 + choice + Sha256K[i] + w[i];
		s0 = Sha256_RotateRight(a, 2U) ^ Sha256_RotateRight(a, 13U) ^
			Sha256_RotateRight(a, 22U);
		majority = (a & b) ^ (a & c) ^ (b & c);
		temp2 = s0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

static void Sha256_Init(Sha256Context_t *ctx)
{
	ctx->state[0] = 0x6A09E667UL;
	ctx->state[1] = 0xBB67AE85UL;
	ctx->state[2] = 0x3C6EF372UL;
	ctx->state[3] = 0xA54FF53AUL;
	ctx->state[4] = 0x510E527FUL;
	ctx->state[5] = 0x9B05688CUL;
	ctx->state[6] = 0x1F83D9ABUL;
	ctx->state[7] = 0x5BE0CD19UL;
	ctx->totalBytes = 0U;
	ctx->blockLength = 0U;
}

static void Sha256_Update(Sha256Context_t *ctx, const uint8_t *data, uint32_t length)
{
	uint32_t copyLength;

	ctx->totalBytes += length;
	while (length > 0U)
	{
		copyLength = SHA256_BLOCK_SIZE - ctx->blockLength;
		if (copyLength > length)
		{
			copyLength = length;
		}
		memcpy(&ctx->block[ctx->blockLength], data, copyLength);
		ctx->blockLength += copyLength;
		data += copyLength;
		length -= copyLength;

		if (ctx->blockLength == SHA256_BLOCK_SIZE)
		{
			Sha256_Transform(ctx, ctx->block);
			ctx->blockLength = 0U;
		}
	}
}

static void Sha256_Final(Sha256Context_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
	uint64_t totalBits;
	uint32_t i;

	totalBits = ctx->totalBytes * 8U;
	ctx->block[ctx->blockLength++] = 0x80U;

	if (ctx->blockLength > 56U)
	{
		while (ctx->blockLength < SHA256_BLOCK_SIZE)
		{
			ctx->block[ctx->blockLength++] = 0U;
		}
		Sha256_Transform(ctx, ctx->block);
		ctx->blockLength = 0U;
	}
	while (ctx->blockLength < 56U)
	{
		ctx->block[ctx->blockLength++] = 0U;
	}
	for (i = 0U; i < 8U; i++)
	{
		ctx->block[63U - i] = (uint8_t)(totalBits >> (i * 8U));
	}
	Sha256_Transform(ctx, ctx->block);

	for (i = 0U; i < 8U; i++)
	{
		Sha256_WriteBigEndian32(&digest[i * 4U], ctx->state[i]);
	}
}

static uint8_t OtaAuth_HexDigit(char ch, uint8_t *value)
{
	if ((ch >= '0') && (ch <= '9'))
	{
		*value = (uint8_t)(ch - '0');
		return 1U;
	}
	if ((ch >= 'a') && (ch <= 'f'))
	{
		*value = (uint8_t)(ch - 'a' + 10);
		return 1U;
	}
	if ((ch >= 'A') && (ch <= 'F'))
	{
		*value = (uint8_t)(ch - 'A' + 10);
		return 1U;
	}
	return 0U;
}

static uint8_t OtaAuth_ParseHex(const char *text, uint8_t *output, uint32_t outputSize)
{
	uint32_t i;
	uint8_t high;
	uint8_t low;

	if ((text == 0) || (output == 0))
	{
		return 0U;
	}
	if (strlen(text) != (outputSize * 2U))
	{
		return 0U;
	}
	for (i = 0U; i < outputSize; i++)
	{
		if (!OtaAuth_HexDigit(text[i * 2U], &high) ||
			!OtaAuth_HexDigit(text[i * 2U + 1U], &low))
		{
			return 0U;
		}
		output[i] = (uint8_t)((high << 4U) | low);
	}
	return (text[outputSize * 2U] == '\0') ? 1U : 0U;
}

static uint8_t OtaAuth_LoadKey(uint8_t key[OTA_AUTH_KEY_SIZE])
{
	return OtaAuth_ParseHex(OTA_FIRMWARE_HMAC_KEY_HEX, key, OTA_AUTH_KEY_SIZE);
}

static void HmacSha256_Init(HmacSha256Context_t *ctx, const uint8_t key[OTA_AUTH_KEY_SIZE])
{
	uint8_t innerPad[SHA256_BLOCK_SIZE];
	uint32_t i;

	memset(innerPad, 0x36, sizeof(innerPad));
	memset(ctx->outerPad, 0x5C, sizeof(ctx->outerPad));
	for (i = 0U; i < OTA_AUTH_KEY_SIZE; i++)
	{
		innerPad[i] ^= key[i];
		ctx->outerPad[i] ^= key[i];
	}
	Sha256_Init(&ctx->inner);
	Sha256_Update(&ctx->inner, innerPad, sizeof(innerPad));
	memset(innerPad, 0, sizeof(innerPad));
}

static void HmacSha256_Update(HmacSha256Context_t *ctx, const uint8_t *data, uint32_t length)
{
	Sha256_Update(&ctx->inner, data, length);
}

static void OtaAuth_AddManifestFields(HmacSha256Context_t *ctx,
	uint32_t version,
	uint32_t size)
{
	static const uint8_t domain[] = "STM32OTA1";
	uint8_t metadata[8];

	/*
	 * 认证内容不仅包含bin，还包含版本和大小。攻击者因此不能把一份合法旧固件
	 * 改写成更高版本号来绕过防降级策略。整数固定按小端序加入HMAC。
	 */
	metadata[0] = (uint8_t)version;
	metadata[1] = (uint8_t)(version >> 8);
	metadata[2] = (uint8_t)(version >> 16);
	metadata[3] = (uint8_t)(version >> 24);
	metadata[4] = (uint8_t)size;
	metadata[5] = (uint8_t)(size >> 8);
	metadata[6] = (uint8_t)(size >> 16);
	metadata[7] = (uint8_t)(size >> 24);
	HmacSha256_Update(ctx, domain, sizeof(domain) - 1U);
	HmacSha256_Update(ctx, metadata, sizeof(metadata));
}

static void HmacSha256_Final(HmacSha256Context_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
	uint8_t innerDigest[SHA256_DIGEST_SIZE];
	Sha256Context_t outer;

	Sha256_Final(&ctx->inner, innerDigest);
	Sha256_Init(&outer);
	Sha256_Update(&outer, ctx->outerPad, sizeof(ctx->outerPad));
	Sha256_Update(&outer, innerDigest, sizeof(innerDigest));
	Sha256_Final(&outer, digest);
	memset(innerDigest, 0, sizeof(innerDigest));
}

static uint8_t OtaAuth_TagsEqual(const uint8_t *left, const uint8_t *right)
{
	uint8_t difference = 0U;
	uint32_t i;

	/* 固定比较全部16字节，避免memcmp在第一个不同字节提前返回造成时序泄漏。 */
	for (i = 0U; i < OTA_AUTH_TAG_SIZE; i++)
	{
		difference |= (uint8_t)(left[i] ^ right[i]);
	}
	return (difference == 0U) ? 1U : 0U;
}

static uint8_t OtaAuth_SelfTest(void)
{
	static uint8_t result = 0U; /* 0=未测试，1=通过，2=失败。 */
	static const uint8_t expected[OTA_AUTH_TAG_SIZE] = {
		0x4D, 0x49, 0xF3, 0xFC, 0x5D, 0x4B, 0xCB, 0x68,
		0x17, 0xB3, 0x14, 0x72, 0x8B, 0xCF, 0xF6, 0xFC
	};
	HmacSha256Context_t hmac;
	uint8_t key[OTA_AUTH_KEY_SIZE];
	uint8_t message[256];
	uint8_t digest[SHA256_DIGEST_SIZE];
	uint32_t i;

	if (result != 0U)
	{
		return (result == 1U) ? 1U : 0U;
	}
	for (i = 0U; i < sizeof(key); i++)
	{
		key[i] = (uint8_t)i;
	}
	for (i = 0U; i < sizeof(message); i++)
	{
		message[i] = (uint8_t)i;
	}
	HmacSha256_Init(&hmac, key);
	OtaAuth_AddManifestFields(&hmac, 7U, sizeof(message));
	HmacSha256_Update(&hmac, message, sizeof(message));
	HmacSha256_Final(&hmac, digest);
	result = OtaAuth_TagsEqual(digest, expected) ? 1U : 2U;
	memset(key, 0, sizeof(key));
	memset(digest, 0, sizeof(digest));
	return (result == 1U) ? 1U : 0U;
}

uint8_t OtaAuth_ParseTagHex(const char *text, uint8_t tag[OTA_AUTH_TAG_SIZE])
{
	return OtaAuth_ParseHex(text, tag, OTA_AUTH_TAG_SIZE);
}

uint8_t OtaAuth_IsTagPresent(const uint8_t tag[OTA_AUTH_TAG_SIZE])
{
	uint32_t i;
	uint8_t value = 0U;

	if (tag == 0)
	{
		return 0U;
	}
	for (i = 0U; i < OTA_AUTH_TAG_SIZE; i++)
	{
		value |= tag[i];
	}
	return (value != 0U) ? 1U : 0U;
}

uint8_t OtaAuth_IsConfigured(void)
{
	uint8_t key[OTA_AUTH_KEY_SIZE];
	uint8_t valid = OtaAuth_SelfTest() && OtaAuth_LoadKey(key);

	memset(key, 0, sizeof(key));
	return valid;
}

uint8_t OtaAuth_CalculateInternalImage(uint32_t address,
	uint32_t size,
	uint32_t version,
	uint8_t tag[OTA_AUTH_TAG_SIZE])
{
	HmacSha256Context_t hmac;
	uint8_t key[OTA_AUTH_KEY_SIZE];
	uint8_t digest[SHA256_DIGEST_SIZE];
	uint32_t offset = 0U;
	uint32_t length;

	if ((size == 0U) || (tag == 0) || !OtaAuth_SelfTest() || !OtaAuth_LoadKey(key))
	{
		return 0U;
	}
	HmacSha256_Init(&hmac, key);
	memset(key, 0, sizeof(key));
	OtaAuth_AddManifestFields(&hmac, version, size);

	while (offset < size)
	{
		length = size - offset;
		if (length > OTA_AUTH_READ_SIZE)
		{
			length = OTA_AUTH_READ_SIZE;
		}
		memcpy(OtaAuth_ReadBuffer, (const void *)(address + offset), length);
		HmacSha256_Update(&hmac, OtaAuth_ReadBuffer, length);
		offset += length;
	}
	HmacSha256_Final(&hmac, digest);
	memcpy(tag, digest, OTA_AUTH_TAG_SIZE);
	memset(digest, 0, sizeof(digest));
	return 1U;
}

uint8_t OtaAuth_VerifyExternalImage(uint32_t address,
	uint32_t size,
	uint32_t version,
	const uint8_t expectedTag[OTA_AUTH_TAG_SIZE])
{
	HmacSha256Context_t hmac;
	uint8_t key[OTA_AUTH_KEY_SIZE];
	uint8_t digest[SHA256_DIGEST_SIZE];
	uint32_t offset = 0U;
	uint32_t length;
	uint8_t valid;

	if ((size == 0U) || !OtaAuth_IsTagPresent(expectedTag) ||
		!OtaAuth_SelfTest() || !OtaAuth_LoadKey(key))
	{
		return 0U;
	}
	HmacSha256_Init(&hmac, key);
	memset(key, 0, sizeof(key));
	OtaAuth_AddManifestFields(&hmac, version, size);

	while (offset < size)
	{
		length = size - offset;
		if (length > OTA_AUTH_READ_SIZE)
		{
			length = OTA_AUTH_READ_SIZE;
		}
		W25Q64_ReadData(address + offset, OtaAuth_ReadBuffer, length);
		HmacSha256_Update(&hmac, OtaAuth_ReadBuffer, length);
		offset += length;
	}
	HmacSha256_Final(&hmac, digest);
	valid = OtaAuth_TagsEqual(digest, expectedTag);
	memset(digest, 0, sizeof(digest));
	return valid;
}
