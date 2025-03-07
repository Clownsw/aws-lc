/* Copyright (c) 2015, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <openssl/ssl.h>

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/mem.h>

#include "../crypto/internal.h"
#include "internal.h"


BSSL_NAMESPACE_BEGIN

// BIO uses int instead of size_t. No lengths will exceed uint16_t, so this will
// not overflow.
static_assert(0xffff <= INT_MAX, "uint16_t does not fit in int");

static_assert((SSL3_ALIGN_PAYLOAD & (SSL3_ALIGN_PAYLOAD - 1)) == 0,
              "SSL3_ALIGN_PAYLOAD must be a power of 2");

void SSLBuffer::Clear() {
  if (buf_allocated_) {
    free(buf_);  // Allocated with malloc().
  }
  buf_ = nullptr;
  buf_allocated_ = false;
  offset_ = 0;
  size_ = 0;
  cap_ = 0;
  buf_size_ = 0;
}

bool SSLBuffer::EnsureCap(size_t header_len, size_t new_cap) {
  if (new_cap > 0xffff) {
    OPENSSL_PUT_ERROR(SSL, ERR_R_INTERNAL_ERROR);
    return false;
  }

  if (cap_ >= new_cap) {
    return true;
  }

  uint8_t *new_buf;
  bool new_buf_allocated;
  size_t new_offset;
  if (new_cap <= sizeof(inline_buf_)) {
    // This function is called twice per TLS record, first for the five-byte
    // header. To avoid allocating twice, use an inline buffer for short inputs.
    new_buf = inline_buf_;
    new_buf_allocated = false;
    new_offset = 0;
  } else {
    // Add up to |SSL3_ALIGN_PAYLOAD| - 1 bytes of slack for alignment.
    //
    // Since this buffer gets allocated quite frequently and doesn't contain any
    // sensitive data, we allocate with malloc rather than |OPENSSL_malloc| and
    // avoid zeroing on free.
    buf_size_ = new_cap + SSL3_ALIGN_PAYLOAD - 1;
    new_buf = (uint8_t *)malloc(buf_size_);
    if (new_buf == NULL) {
      OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
      return false;
    }
    new_buf_allocated = true;

    // Offset the buffer such that the record body is aligned.
    new_offset =
        (0 - header_len - (uintptr_t)new_buf) & (SSL3_ALIGN_PAYLOAD - 1);
  }

  // Note if the both old and new buffer are inline, the source and destination
  // may alias.
  OPENSSL_memmove(new_buf + new_offset, buf_ + offset_, size_);

  if (buf_allocated_) {
    free(buf_);  // Allocated with malloc().
  }

  buf_ = new_buf;
  buf_allocated_ = new_buf_allocated;
  offset_ = new_offset;
  cap_ = new_cap;
  return true;
}

void SSLBuffer::DidWrite(size_t new_size) {
  if (new_size > cap() - size()) {
    abort();
  }
  size_ += new_size;
}

void SSLBuffer::Consume(size_t len) {
  if (len > size_) {
    abort();
  }
  offset_ += (uint16_t)len;
  size_ -= (uint16_t)len;
  cap_ -= (uint16_t)len;
}

void SSLBuffer::DiscardConsumed() {
  if (size_ == 0) {
    Clear();
  }
}

// An SSLBuffer is serialized as the following ASN.1 structure:
//
// SSLBuffer ::= SEQUENCE {
//    version                           INTEGER (1),  -- SSLBuffer structure version
//    bufAllocated                      BOOLEAN,
//    offset                            INTEGER,
//    size                              INTEGER,
//    cap                               INTEGER,
//    buf                               OCTET STRING,
// }
static const unsigned kSSLBufferVersion = 1;

bool SSLBuffer::DoSerialization(CBB *cbb) {
  if (!cbb) {
    return false;
  }
  CBB seq;
  if (!CBB_add_asn1(cbb, &seq, CBS_ASN1_SEQUENCE) ||
      !CBB_add_asn1_uint64(&seq, kSSLBufferVersion) ||
      !CBB_add_asn1_bool(&seq, (buf_allocated_ ? 1 : 0)) ||
      !CBB_add_asn1_uint64(&seq, offset_) ||
      !CBB_add_asn1_uint64(&seq, size_) ||
      !CBB_add_asn1_uint64(&seq, cap_) ||
      (buf_allocated_ && !CBB_add_asn1_octet_string(&seq, buf_, buf_size_)) ||
      (!buf_allocated_ && !CBB_add_asn1_octet_string(&seq, inline_buf_, SSL3_RT_HEADER_LENGTH))) {
    OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
    return false;
  }
  return CBB_flush(cbb) == 1;
}

bool SSLBuffer::DoDeserialization(CBS *cbs) {
  if (!cbs) {
    return false;
  }

  CBS seq, buf;
  int buf_allocated_int;
  uint64_t version, offset, size, cap;
  if (!CBS_get_asn1(cbs, &seq, CBS_ASN1_SEQUENCE) ||
      !CBS_get_asn1_uint64(&seq, &version) ||
      version != kSSLBufferVersion ||
      !CBS_get_asn1_bool(&seq, &buf_allocated_int) ||
      !CBS_get_asn1_uint64(&seq, &offset) ||
      !CBS_get_asn1_uint64(&seq, &size) ||
      !CBS_get_asn1_uint64(&seq, &cap) ||
      !CBS_get_asn1(&seq, &buf, CBS_ASN1_OCTETSTRING) ||
      CBS_len(&seq) != 0) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_SERIALIZATION_INVALID_SSL_BUFFER);
    return false;
  }
  bool buf_allocated = !!buf_allocated_int;
  if (buf_allocated) {
    // When buf_allocated, CBS_len(&buf) should be larger than
    // sizeof(inline_buf_). This is ensured in |EnsureCap|.
    if (CBS_len(&buf) <= sizeof(inline_buf_)) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_SERIALIZATION_INVALID_SSL_BUFFER);
      return false;
    }
    buf_ = (uint8_t *)malloc(CBS_len(&buf));
    if (buf_ == NULL) {
      OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
      return false;
    }
    buf_size_ = CBS_len(&buf);
    OPENSSL_memcpy(buf_, CBS_data(&buf), CBS_len(&buf));
  } else {
    if (CBS_len(&buf) != sizeof(inline_buf_)) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_SERIALIZATION_INVALID_SSL_BUFFER);
      return false;
    }
    buf_size_ = 0;
    OPENSSL_memcpy(inline_buf_, CBS_data(&buf), CBS_len(&buf));
  }
  buf_allocated_ = buf_allocated;
  offset_ = (uint16_t)offset;
  size_ = (uint16_t)size;
  cap_ = (uint16_t)cap;
  return true;
}

static int dtls_read_buffer_next_packet(SSL *ssl) {
  SSLBuffer *buf = &ssl->s3->read_buffer;

  if (!buf->empty()) {
    // It is an error to call |dtls_read_buffer_extend| when the read buffer is
    // not empty.
    OPENSSL_PUT_ERROR(SSL, ERR_R_INTERNAL_ERROR);
    return -1;
  }

  // Read a single packet from |ssl->rbio|. |buf->cap()| must fit in an int.
  int ret =
      BIO_read(ssl->rbio.get(), buf->data(), static_cast<int>(buf->cap()));
  if (ret <= 0) {
    ssl->s3->rwstate = SSL_ERROR_WANT_READ;
    return ret;
  }
  buf->DidWrite(static_cast<size_t>(ret));
  return 1;
}

static int tls_read_buffer_extend_to(SSL *ssl, size_t len) {
  SSLBuffer *buf = &ssl->s3->read_buffer;

  if (len > buf->cap()) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_BUFFER_TOO_SMALL);
    return -1;
  }

  // Read until the target length is reached.
  while (buf->size() < len) {
    // The amount of data to read is bounded by |buf->cap|, which must fit in an
    // int.
    int ret = BIO_read(ssl->rbio.get(), buf->data() + buf->size(),
                       static_cast<int>(len - buf->size()));
    if (ret <= 0) {
      ssl->s3->rwstate = SSL_ERROR_WANT_READ;
      return ret;
    }
    buf->DidWrite(static_cast<size_t>(ret));
  }

  return 1;
}

int ssl_read_buffer_extend_to(SSL *ssl, size_t len) {
  // |ssl_read_buffer_extend_to| implicitly discards any consumed data.
  ssl->s3->read_buffer.DiscardConsumed();

  if (SSL_is_dtls(ssl)) {
    static_assert(
        DTLS1_RT_HEADER_LENGTH + SSL3_RT_MAX_ENCRYPTED_LENGTH <= 0xffff,
        "DTLS read buffer is too large");

    // The |len| parameter is ignored in DTLS.
    len = DTLS1_RT_HEADER_LENGTH + SSL3_RT_MAX_ENCRYPTED_LENGTH;
  }

  if (!ssl->s3->read_buffer.EnsureCap(ssl_record_prefix_len(ssl), len)) {
    return -1;
  }

  if (ssl->rbio == nullptr) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_BIO_NOT_SET);
    return -1;
  }

  int ret;
  if (SSL_is_dtls(ssl)) {
    // |len| is ignored for a datagram transport.
    ret = dtls_read_buffer_next_packet(ssl);
  } else {
    ret = tls_read_buffer_extend_to(ssl, len);
  }

  if (ret <= 0) {
    // If the buffer was empty originally and remained empty after attempting to
    // extend it, release the buffer until the next attempt.
    ssl->s3->read_buffer.DiscardConsumed();
  }
  return ret;
}

int ssl_handle_open_record(SSL *ssl, bool *out_retry, ssl_open_record_t ret,
                           size_t consumed, uint8_t alert) {
  *out_retry = false;
  if (ret != ssl_open_record_partial) {
    ssl->s3->read_buffer.Consume(consumed);
  }
  if (ret != ssl_open_record_success) {
    // Nothing was returned to the caller, so discard anything marked consumed.
    ssl->s3->read_buffer.DiscardConsumed();
  }
  switch (ret) {
    case ssl_open_record_success:
      return 1;

    case ssl_open_record_partial: {
      int read_ret = ssl_read_buffer_extend_to(ssl, consumed);
      if (read_ret <= 0) {
        return read_ret;
      }
      *out_retry = true;
      return 1;
    }

    case ssl_open_record_discard:
      *out_retry = true;
      return 1;

    case ssl_open_record_close_notify:
      ssl->s3->rwstate = SSL_ERROR_ZERO_RETURN;
      return 0;

    case ssl_open_record_error:
      if (alert != 0) {
        ssl_send_alert(ssl, SSL3_AL_FATAL, alert);
      }
      return -1;
  }
  assert(0);
  return -1;
}


static_assert(SSL3_RT_HEADER_LENGTH * 2 +
                      SSL3_RT_SEND_MAX_ENCRYPTED_OVERHEAD * 2 +
                      SSL3_RT_MAX_PLAIN_LENGTH <=
                  0xffff,
              "maximum TLS write buffer is too large");

static_assert(DTLS1_RT_HEADER_LENGTH + SSL3_RT_SEND_MAX_ENCRYPTED_OVERHEAD +
                      SSL3_RT_MAX_PLAIN_LENGTH <=
                  0xffff,
              "maximum DTLS write buffer is too large");

static int tls_write_buffer_flush(SSL *ssl) {
  SSLBuffer *buf = &ssl->s3->write_buffer;

  while (!buf->empty()) {
    int ret = BIO_write(ssl->wbio.get(), buf->data(), buf->size());
    if (ret <= 0) {
      ssl->s3->rwstate = SSL_ERROR_WANT_WRITE;
      return ret;
    }
    buf->Consume(static_cast<size_t>(ret));
  }
  buf->Clear();
  return 1;
}

static int dtls_write_buffer_flush(SSL *ssl) {
  SSLBuffer *buf = &ssl->s3->write_buffer;
  if (buf->empty()) {
    return 1;
  }

  int ret = BIO_write(ssl->wbio.get(), buf->data(), buf->size());
  if (ret <= 0) {
    ssl->s3->rwstate = SSL_ERROR_WANT_WRITE;
    // If the write failed, drop the write buffer anyway. Datagram transports
    // can't write half a packet, so the caller is expected to retry from the
    // top.
    buf->Clear();
    return ret;
  }
  buf->Clear();
  return 1;
}

int ssl_write_buffer_flush(SSL *ssl) {
  if (ssl->wbio == nullptr) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_BIO_NOT_SET);
    return -1;
  }

  if (SSL_is_dtls(ssl)) {
    return dtls_write_buffer_flush(ssl);
  } else {
    return tls_write_buffer_flush(ssl);
  }
}

BSSL_NAMESPACE_END
