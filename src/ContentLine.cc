// $Id: ContentLine.cc,v 1.1.2.8 2006/06/01 01:55:42 sommer Exp $

#include <algorithm>

#include "ContentLine.h"
#include "TCP.h"

ContentLine_Analyzer::ContentLine_Analyzer(Connection* conn, bool orig)
: TCP_SupportAnalyzer(AnalyzerTag::ContentLine, conn, orig)
	{
	InitState();
	}

ContentLine_Analyzer::ContentLine_Analyzer(AnalyzerTag::Tag tag,
						Connection* conn, bool orig)
: TCP_SupportAnalyzer(tag, conn, orig)
	{
	InitState();
	}

void ContentLine_Analyzer::InitState()
	{
	flag_NULs = 0;
	CR_LF_as_EOL = (CR_as_EOL | LF_as_EOL);
	skip_deliveries = 0;
	skip_partial = 0;
	buf = 0;
	seq_delivered_in_lines = 0;
	skip_pending = 0;
	seq = 0;
	seq_to_skip = 0;
	plain_delivery_length = 0;
	is_plain = 0;

	InitBuffer(0);
	}

void ContentLine_Analyzer::InitBuffer(int size)
	{
	if ( buf && buf_len >= size )
		// Don't shrink the buffer, because it's not clear in that
		// case how to deal with characters in it that no longer fit.
		return;

	if ( size < 128 )
		size = 128;

	u_char* b = new u_char[size];

	if ( buf )
		{
		if ( offset > 0 )
			memcpy(b, buf, offset);
		delete [] buf;
		}
	else
		{
		offset = 0;
		last_char = 0;
		}

	buf = b;
	buf_len = size;
	}

ContentLine_Analyzer::~ContentLine_Analyzer()
	{
	delete [] buf;
	}

int ContentLine_Analyzer::HasPartialLine() const
	{
	return buf && offset > 0;
	}

void ContentLine_Analyzer::DeliverStream(int len, const u_char* data,
						bool is_orig)
	{
	TCP_SupportAnalyzer::DeliverStream(len, data, is_orig);

	if ( len <= 0 || SkipDeliveries() )
		return;

	if ( skip_partial )
		{
		TCP_Analyzer* tcp =
			static_cast<TCP_ApplicationAnalyzer*>(Parent())->TCP();

		if ( tcp && tcp->IsPartial() )
			return;
		}

	if ( buf && len + offset >= buf_len )
		{ // Make sure we have enough room to accommodate the new stuff.
		int old_buf_len = buf_len;
		buf_len = ((offset + len) * 3) / 2 + 1;

		u_char* tmp = new u_char[buf_len];
		for ( int i = 0; i < old_buf_len; ++i )
			tmp[i] = buf[i];

		delete [] buf;
		buf = tmp;

		if ( ! buf )
			bro_logger->InternalError("out of memory delivering endpoint line");
		}

	DoDeliver(len, data);

	seq += len;
	}

void ContentLine_Analyzer::Undelivered(int seq, int len, bool orig)
	{
	ForwardUndelivered(seq, len, orig);
	}

void ContentLine_Analyzer::EndpointEOF(bool is_orig)
	{
	if ( offset > 0 )
		DeliverStream(1, (const u_char*) "\n", is_orig);
	}

void ContentLine_Analyzer::SetPlainDelivery(int64_t length)
	{
	if ( length < 0 )
		bro_logger->InternalError("negative length for plain delivery");

	plain_delivery_length = length;
	}

void ContentLine_Analyzer::DoDeliver(int len, const u_char* data)
	{
	seq_delivered_in_lines = seq;

	while ( len > 0 && ! SkipDeliveries() )
		{
		if ( (CR_LF_as_EOL & CR_as_EOL) &&
		     last_char == '\r' && *data == '\n' )
			{
			// CR is already considered as EOL.
			// Compress CRLF to just one line termination.
			//
			// Note, we test this prior to checking for
			// "plain delivery" because (1) we might have
			// made the decision to switch to plain delivery
			// based on a line terminated with '\r' for
			// which a '\n' then arrived, and (2) we are
			// careful when executing plain delivery to
			// clear last_char once we do so.
			last_char = *data;
			--len; ++data; ++seq;
			++seq_delivered_in_lines;
			}

		if ( plain_delivery_length > 0 )
			{
			int deliver_plain = min(plain_delivery_length, (int64_t)len);

			last_char = 0; // clear last_char
			plain_delivery_length -= deliver_plain;
			is_plain = 1;

			ForwardStream(deliver_plain, data, IsOrig());

			is_plain = 0;

			data += deliver_plain;
			len -= deliver_plain;
			if ( len == 0 )
				return;
			}

		if ( skip_pending > 0 )
			SkipBytes(skip_pending);

		// Note that the skipping must take place *after*
		// the CR/LF check above, so that the '\n' of the
		// previous line is skipped first.
		if ( seq < seq_to_skip )
			{
			// Skip rest of the data and return
			int64_t skip_len = seq_to_skip - seq;
			if ( skip_len > len )
				skip_len = len;

			ForwardUndelivered(seq, skip_len, IsOrig());

			len -= skip_len; data += skip_len; seq += skip_len;
			seq_delivered_in_lines += skip_len;
			}

		if ( len <= 0 )
			break;

		int n = DoDeliverOnce(len, data);
		len -= n;
		data += n;
		seq += n;
		}
	}

int ContentLine_Analyzer::DoDeliverOnce(int len, const u_char* data)
	{
	const u_char* data_start = data;

	if ( len <= 0 )
		return 0;

	for ( ; len > 0; --len, ++data )
		{
		if ( offset >= buf_len )
			InitBuffer(buf_len * 2);

		int c = data[0];

#define EMIT_LINE \
	{ \
	buf[offset] = '\0'; \
	int seq_len = data + 1 - data_start; \
	seq_delivered_in_lines = seq + seq_len; \
	last_char = c; \
	ForwardStream(offset, buf, IsOrig()); \
	offset = 0; \
	return seq_len; \
	}

		switch ( c ) {
		case '\r':
			// Look ahead for '\n'.
			if ( len > 1 && data[1] == '\n' )
				{
				--len; ++data;
				last_char = c;
				c = data[0];
				EMIT_LINE
				}

			else if ( CR_LF_as_EOL & CR_as_EOL )
				EMIT_LINE

			else
				buf[offset++] = c;
			break;

		case '\n':
			if ( last_char == '\r' )
				{
				--offset; // remove '\r'
				EMIT_LINE
				}

			else if ( CR_LF_as_EOL & LF_as_EOL )
				EMIT_LINE

			else
				{
				if ( Conn()->FlagEvent(SINGULAR_LF) )
					Conn()->Weird("line_terminated_with_single_LF");
				buf[offset++] = c;
				}
			break;

		case '\0':
			if ( flag_NULs )
				CheckNUL();
			else
				buf[offset++] = c;
			break;

		default:
			buf[offset++] = c;
			break;
		}

		if ( last_char == '\r' )
			if ( Conn()->FlagEvent(SINGULAR_CR) )
				Conn()->Weird("line_terminated_with_single_CR");

		last_char = c;
		}

	return data - data_start;
	}

void ContentLine_Analyzer::CheckNUL()
	{
	// If this is the first byte seen on this connection,
	// and if the connection's state is PARTIAL, then we've
	// intercepted a keep-alive, and shouldn't complain
	// about it.  Note that for PARTIAL connections, the
	// starting sequence number is adjusted as though there
	// had been an initial SYN, so we check for whether
	// the connection has at most two bytes so far.

	TCP_Analyzer* tcp =
		static_cast<TCP_ApplicationAnalyzer*>(Parent())->TCP();

	if ( tcp )
		{
		TCP_Endpoint* endp = IsOrig() ? tcp->Orig() : tcp->Resp();
		if ( endp->state == TCP_ENDPOINT_PARTIAL &&
		     endp->LastSeq() - endp->StartSeq() <= 2 )
			; // Ignore it.
		else
			{
			if ( Conn()->FlagEvent(NUL_IN_LINE) )
				Conn()->Weird("NUL_in_line");
			flag_NULs = 0;
			}
		}
	}

void ContentLine_Analyzer::SkipBytesAfterThisLine(int64_t length)
	{
	// This is a little complicated because Bro has to handle
	// both CR and CRLF as a line break. When a line is delivered,
	// it's possible that only a CR is seen, and we may not know
	// if an LF is following until we see the next packet.  If an
	// LF follows, we should start skipping bytes *after* the LF.
	// So we keep the skip as 'pending' until we see the next
	// character in DoDeliver().

	if ( last_char == '\r' )
		skip_pending = length;
	else
		SkipBytes(length);
	}

void ContentLine_Analyzer::SkipBytes(int64_t length)
	{
	skip_pending = 0;
	seq_to_skip = SeqDelivered() + length;
	}

