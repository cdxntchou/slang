#include "slang-string.h"
#include "slang-text-io.h"

namespace Slang
{
    // TODO: this belongs in a different file:

    SLANG_RETURN_NEVER void signalUnexpectedError(char const* message)
    {
        // Can be useful to uncomment during debug when problem is on CI
        // printf("Unexpected: %s\n", message);
        throw InternalError(message);
    }

    SLANG_FORCE_INLINE static bool _isWhiteSpace(char c)
    {
        return c == ' ' || c == '\t';
    }

    // OSString

    OSString::OSString()
        : m_begin(nullptr)
        , m_end(nullptr)
    {}

    OSString::OSString(wchar_t* begin, wchar_t* end)
        : m_begin(begin)
        , m_end(end)
    {}

    void OSString::_releaseBuffer()
    {
        if (m_begin)
        {
            delete[] m_begin;
        }
    }

    void OSString::set(const wchar_t* begin, const wchar_t* end)
    {
        if (m_begin)
        {
            delete[] m_begin;
            m_begin = nullptr;
            m_end = nullptr;
        }
        const size_t len = end - begin;
        if (len > 0)
        {
            // TODO(JS): The allocation is only done this way to be compatible with the buffer being detached from an array
            // This is unfortunate, because it means that the allocation stores the size (and alignment fix), which is a shame because we know the size
            m_begin = new wchar_t[len + 1];
            memcpy(m_begin, begin, len * sizeof(wchar_t));
            // Zero terminate
            m_begin[len] = 0;
            m_end = m_begin + len;
        }
    }

    static const wchar_t kEmptyOSString[] = { 0 };

    wchar_t const* OSString::begin() const
    {
        return m_begin ? m_begin : kEmptyOSString;
    }

    wchar_t const* OSString::end() const
    {
        return m_end ? m_end : kEmptyOSString;
    }

    // UnownedStringSlice

    bool UnownedStringSlice::startsWith(UnownedStringSlice const& other) const
    {
        UInt thisSize = size();
        UInt otherSize = other.size();

        if (otherSize > thisSize)
            return false;

        return UnownedStringSlice(begin(), begin() + otherSize) == other;
    }

    bool UnownedStringSlice::startsWith(char const* str) const
    {
        return startsWith(UnownedTerminatedStringSlice(str));
    }


    bool UnownedStringSlice::endsWith(UnownedStringSlice const& other) const
    {
        UInt thisSize = size();
        UInt otherSize = other.size();

        if (otherSize > thisSize)
            return false;

        return UnownedStringSlice(
            end() - otherSize, end()) == other;
    }

    bool UnownedStringSlice::endsWith(char const* str) const
    {
        return endsWith(UnownedTerminatedStringSlice(str));
    }

    
    UnownedStringSlice UnownedStringSlice::trim() const
    {
        const char* start = m_begin;
        const char* end = m_end;

        while (start < end && _isWhiteSpace(*start)) start++;
        while (end > start && _isWhiteSpace(end[-1])) end--;
        return UnownedStringSlice(start, end);
    }


    // StringSlice

    StringSlice::StringSlice()
        : representation(0)
        , beginIndex(0)
        , endIndex(0)
    {}

    StringSlice::StringSlice(String const& str)
        : representation(str.m_buffer)
        , beginIndex(0)
        , endIndex(str.getLength())
    {}

    StringSlice::StringSlice(String const& str, UInt beginIndex, UInt endIndex)
        : representation(str.m_buffer)
        , beginIndex(beginIndex)
        , endIndex(endIndex)
    {}


    //

	_EndLine EndLine;

    String operator+(const char * op1, const String & op2)
	{
        String result(op1);
        result.append(op2);
        return result;
	}

	String operator+(const String & op1, const char * op2)
	{
        String result(op1);
        result.append(op2);
        return result;
	}

	String operator+(const String & op1, const String & op2)
	{
        String result(op1);
        result.append(op2);
        return result;
	}

	int StringToInt(const String & str, int radix)
	{
		if (str.startsWith("0x"))
			return (int)strtoll(str.getBuffer(), NULL, 16);
		else
			return (int)strtoll(str.getBuffer(), NULL, radix);
	}
	unsigned int StringToUInt(const String & str, int radix)
	{
		if (str.startsWith("0x"))
			return (unsigned int)strtoull(str.getBuffer(), NULL, 16);
		else
			return (unsigned int)strtoull(str.getBuffer(), NULL, radix);
	}
	double StringToDouble(const String & str)
	{
		return (double)strtod(str.getBuffer(), NULL);
	}
	float StringToFloat(const String & str)
	{
		return strtof(str.getBuffer(), NULL);
	}

#if 0
	String String::ReplaceAll(String src, String dst) const
	{
		String rs = *this;
		int index = 0;
		int srcLen = src.length;
		int len = rs.length;
		while ((index = rs.IndexOf(src, index)) != -1)
		{
			rs = rs.SubString(0, index) + dst + rs.SubString(index + srcLen, len - index - srcLen);
			len = rs.length;
		}
		return rs;
	}
#endif

	String String::fromWString(const wchar_t * wstr)
	{
#ifdef _WIN32
		return Slang::Encoding::UTF16->ToString((const char*)wstr, (int)(wcslen(wstr) * sizeof(wchar_t)));
#else
		return Slang::Encoding::UTF32->ToString((const char*)wstr, (int)(wcslen(wstr) * sizeof(wchar_t)));
#endif
	}

	String String::fromWString(const wchar_t * wstr, const wchar_t * wend)
	{
#ifdef _WIN32
		return Slang::Encoding::UTF16->ToString((const char*)wstr, (int)((wend - wstr) * sizeof(wchar_t)));
#else
		return Slang::Encoding::UTF32->ToString((const char*)wstr, (int)((wend - wstr) * sizeof(wchar_t)));
#endif
	}

	String String::fromWChar(const wchar_t ch)
	{
#ifdef _WIN32
		return Slang::Encoding::UTF16->ToString((const char*)&ch, (int)(sizeof(wchar_t)));
#else
		return Slang::Encoding::UTF32->ToString((const char*)&ch, (int)(sizeof(wchar_t)));
#endif
	}

	String String::fromUnicodePoint(unsigned int codePoint)
	{
		char buf[6];
		int len = Slang::EncodeUnicodePointToUTF8(buf, (int)codePoint);
		buf[len] = 0;
		return String(buf);
	}

	OSString String::toWString(Index* outLength) const
	{
		if (!m_buffer)
		{
            return OSString();
		}
		else
		{
			List<char> buf;
            switch(sizeof(wchar_t))
            {
            case 2:
                Slang::Encoding::UTF16->GetBytes(buf, *this);                
                break;

            case 4:
                Slang::Encoding::UTF32->GetBytes(buf, *this);                
                break;

            default:
                break;
            }

            auto length = Index(buf.getCount() / sizeof(wchar_t));
			if (outLength)
				*outLength = length;

            for(int ii = 0; ii < sizeof(wchar_t); ++ii)
    			buf.add(0);

            wchar_t* beginData = (wchar_t*)buf.getBuffer();
            wchar_t* endData = beginData + length;

			buf.detachBuffer();

            return OSString(beginData, endData);
		}
	}

    //

    void String::ensureUniqueStorageWithCapacity(Index requiredCapacity)
    {
        if (m_buffer && m_buffer->isUniquelyReferenced() && m_buffer->capacity >= requiredCapacity)
            return;

        Index newCapacity = m_buffer ? 2 * m_buffer->capacity : 16;
        if (newCapacity < requiredCapacity)
        {
            newCapacity = requiredCapacity;
        }

        Index length = getLength();
        StringRepresentation* newRepresentation = StringRepresentation::createWithCapacityAndLength(newCapacity, length);

        if (m_buffer)
        {
            memcpy(newRepresentation->getData(), m_buffer->getData(), length + 1);
        }

        m_buffer = newRepresentation;
    }

    char* String::prepareForAppend(Index count)
    {
        auto oldLength = getLength();
        auto newLength = oldLength + count;
        ensureUniqueStorageWithCapacity(newLength);
        return getData() + oldLength;
    }
    void String::appendInPlace(const char* chars, Index count)
    {
        SLANG_UNUSED(chars);

        if (count > 0)
        {
            SLANG_ASSERT(m_buffer && m_buffer->isUniquelyReferenced());

            auto oldLength = getLength();
            auto newLength = oldLength + count;

            char* dst = m_buffer->getData();

            // Make sure the input buffer is the same one returned from prepareForAppend
            SLANG_ASSERT(chars == dst + oldLength);
            // It has to fit within the capacity
            SLANG_ASSERT(newLength <= m_buffer->capacity);

            // We just need to modify the length
            m_buffer->length = newLength;

            // And mark with a terminating 0
            dst[newLength] = 0;
        }
    }

    void String::append(const char* textBegin, char const* textEnd)
    {
        auto oldLength = getLength();
        auto textLength = textEnd - textBegin;

        auto newLength = oldLength + textLength;

        ensureUniqueStorageWithCapacity(newLength);

        memcpy(getData() + oldLength, textBegin, textLength);
        getData()[newLength] = 0;
        m_buffer->length = newLength;
    }

    void String::append(char const* str)
    {
        if (str)
        {
            append(str, str + strlen(str));
        }
    }

    void String::append(char chr)
    {
        append(&chr, &chr + 1);
    }

    void String::append(String const& str)
    {
        if (!m_buffer)
        {
            m_buffer = str.m_buffer;
            return;
        }

        append(str.begin(), str.end());
    }

    void String::append(StringSlice const& slice)
    {
        append(slice.begin(), slice.end());
    }

    void String::append(UnownedStringSlice const& slice)
    {
        append(slice.begin(), slice.end());
    }

    void String::append(int32_t value, int radix)
    {
        enum { kCount = 33 };
        char* data = prepareForAppend(kCount);
        auto count = IntToAscii(data, value, radix);
        ReverseInternalAscii(data, count);
        m_buffer->length += count;
    }

    void String::append(uint32_t value, int radix)
    {
        enum { kCount = 33 };
        char* data = prepareForAppend(kCount);
        auto count = IntToAscii(data, value, radix);
        ReverseInternalAscii(data, count);
        m_buffer->length += count;
    }

    void String::append(int64_t value, int radix)
    {
        enum { kCount = 65 };
        char* data = prepareForAppend(kCount);
        auto count = IntToAscii(data, value, radix);
        ReverseInternalAscii(data, count);
        m_buffer->length += count;
    }

    void String::append(uint64_t value, int radix)
    {
        enum { kCount = 65 };
        char* data = prepareForAppend(kCount);
        auto count = IntToAscii(data, value, radix);
        ReverseInternalAscii(data, count);
        m_buffer->length += count;
    }

    void String::append(float val, const char * format)
    {
        enum { kCount = 128 };
        char* data = prepareForAppend(kCount);
        sprintf_s(data, kCount, format, val);
        m_buffer->length += strnlen_s(data, kCount);
    }

    void String::append(double val, const char * format)
    {
        enum { kCount = 128 };
        char* data = prepareForAppend(kCount);
        sprintf_s(data, kCount, format, val);
        m_buffer->length += strnlen_s(data, kCount);
    }
}
