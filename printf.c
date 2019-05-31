///////////////////////////////////////////////////////////////////////////////
// \author (c) Marco Paland (info@paland.com)
//             2014-2019, PALANDesign Hannover, Germany
//
// \license The MIT License (MIT)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// \brief Tiny printf, sprintf and (v)snprintf implementation, optimized for speed on
//        embedded systems with a very limited resources. These routines are thread
//        safe and reentrant!
//        Use this instead of the bloated standard/newlib printf cause these use
//        malloc for printf (and may not be thread safe).
//
///////////////////////////////////////////////////////////////////////////////

/* Optimized for low stack use on ARM. Support for floating point has been removed. 
 *	It should use less than 100 bytes on the stack on 32 bit ARM. 
 */

/* Formatted output with the following format specifier: %[flags][width][.precision][length]type
Supported Types
Type	Output
d or i	Signed decimal integer
u				Unsigned decimal integer
b				Unsigned binary
o				Unsigned octal
x				Unsigned hexadecimal integer (lowercase)
X				Unsigned hexadecimal integer (uppercase)
c				Single character
s				String of characters
p				Pointer address
%				A % followed by another % character will write a single %

Supported Flags
Flags	Description
-			Left-justify within the given field width; Right justification is the default.
+			Forces to precede the result with a plus or minus sign (+ or -) even for positive numbers.
			By default, only negative numbers are preceded with a - sign.
			(space)	If no sign is going to be written, a blank space is inserted before the value.
#			Used with o, b, x or X specifiers the value is preceded with 0, 0b, 0x or 0X respectively for values different than zero.
			Used with f, F it forces the written output to contain a decimal point even if no more digits follow. By default, if no digits follow, no decimal point is written.
0			Left-pads the number with zeros (0) instead of spaces when padding is specified (see width sub-specifier).

Supported Width
Width	Description
(number)	Minimum number of characters to be printed. If the value to be printed is shorter than this number, the result is padded with blank spaces. The value is not truncated even if the result is larger.
*					The width is not specified in the format string, but as an additional integer value argument preceding the argument that has to be formatted.

Supported Precision
Precision	Description
.number	For integer specifiers (d, i, o, u, x, X): precision specifies the minimum number of digits to be written. If the value to be written is shorter than this number, the result is padded with leading zeros. The value is not truncated even if the result is longer. A precision of 0 means that no character is written for the value 0.
				For f and F specifiers: this is the number of digits to be printed after the decimal point. By default, this is 6, maximum is 9.
				For s: this is the maximum number of characters to be printed. By default all characters are printed until the ending null character is encountered.
				If the period is specified without an explicit value for precision, 0 is assumed.
.*			The precision is not specified in the format string, but as an additional integer value argument preceding the argument that has to be formatted.

Supported Length
The length sub-specifier modifies the length of the data type.
Length	d i						u o x X
(none)	int						unsigned int
hh			char					unsigned char
h				short int			unsigned short int
l				long int			unsigned long int
ll			long long int	unsigned long long int (if PRINTF_SUPPORT_LONG_LONG is defined)
j				intmax_t			uintmax_t
z				size_t				size_t
t				ptrdiff_t			ptrdiff_t (if PRINTF_SUPPORT_PTRDIFF_T is defined)
*/

#include <stdbool.h>
#include <stdint.h>
#include "printf.h"

// support for the long long types (%llu or %p)
#define PRINTF_SUPPORT_LONG_LONG

// support for the ptrdiff_t type (%t)
// ptrdiff_t is normally defined in <stddef.h> as long or long long type
#define PRINTF_SUPPORT_PTRDIFF_T

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ABS(a) ((a) < 0 ? -(a) : (a))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

///////////////////////////////////////////////////////////////////////////////

// internal flag definitions
#define FLAGS_ZEROPAD   (1U <<  0U)
#define FLAGS_LEFT      (1U <<  1U)
#define FLAGS_PLUS      (1U <<  2U)
#define FLAGS_SPACE     (1U <<  3U)
#define FLAGS_HASH      (1U <<  4U)
#define FLAGS_UPPERCASE (1U <<  5U)
#define FLAGS_CHAR      (1U <<  6U)
#define FLAGS_SHORT     (1U <<  7U)
#define FLAGS_LONG      (1U <<  8U)
#define FLAGS_LONG_LONG (1U <<  9U)
#define FLAGS_PRECISION (1U << 10U)
#define FLAGS_ADAPT_EXP (1U << 11U)
#define FLAGS_NEGATIVE  (1U << 12U)

typedef enum {BASE_BINARY, BASE_OCTAL, BASE_DECIMAL, BASE_HEX} base_t;

typedef struct
{
	out_fct_type out;
	void* ptr;
	uint16_t flags;
	uint8_t idx;
	uint8_t width;
	uint8_t precision;
	base_t base;
	uint8_t digit_count;
} data_t;

typedef struct 
{
	char *buffer;
	size_t maxlen;
} sprintf_data_t;

static const unsigned long pow10_long[] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };
#ifdef PRINTF_SUPPORT_LONG_LONG
static const unsigned long long pow10_long_long[] = { 1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL, 100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL, 1000000000000ULL, 10000000000000ULL, 100000000000000ULL, 1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL, 1000000000000000000ULL, 10000000000000000000ULL };
#endif
static const uint8_t base_bits[] = { 1, 3, 0, 4 }; //Number of bits for every base in base_t
static const uint8_t base_mask[] = { 1, 7, 0, 15 }; //Mask for a digit in every base in base_t

static void out_sprintf(char character, void* ptr)
{
	sprintf_data_t *data = (sprintf_data_t*)ptr;
  if(data->maxlen > 0 && data->buffer != NULL) 
	{
    *data->buffer++ = character;
		data->maxlen--;
  }
}

// internal _putchar wrapper
static inline void out_char(char character, void* ptr)
{
  (void)ptr; 
  if (character) 
	{
    _putchar(character);
  }
}

// internal secure strlen
// \return The length of the string (excluding the terminating 0) limited by 'maxsize'
static inline unsigned int strnlen_s(const char* str, size_t maxsize)
{
  const char* s = str;
  while(*s != 0 && maxsize > 0)
	{
		++s;
		--maxsize;
	}
  return (unsigned int)(s - str);
}


// internal test if char is a digit (0-9)
// \return true if char is a digit
static inline bool is_digit(char ch)
{
  return (ch >= '0') && (ch <= '9');
}

// internal ASCII string to unsigned int conversion
static unsigned int atoi(const char** str)
{
  unsigned int i = 0U;
  while (is_digit(**str)) 
	{
    i = i * 10U + (unsigned int)(*((*str)++) - '0');
  }
  return i;
}

// internal itoa format
static void ntoa_format(data_t *data)
{
	data->width = MAX(data->width - data->digit_count, 0);
	data->precision = MAX(data->precision - data->digit_count, 0);
	if (data->width > 0 && (data->flags & (FLAGS_NEGATIVE | FLAGS_PLUS | FLAGS_SPACE)))
	{
		data->width--;
	}

	if (data->flags & FLAGS_PRECISION)
	{
		data->width = MAX(data->width - data->precision, 0);
	}
	
	if (data->flags & FLAGS_HASH) 
	{
		if (data->base == BASE_HEX || data->base == BASE_BINARY) 
		{
			data->width = MAX(data->width - 2, 0);
		}
		else if(data->width > 0)
		{
			data->width--;
		}
	}

	// pad spaces up to given width
	if (!(data->flags & FLAGS_LEFT) && !(data->flags & FLAGS_ZEROPAD)) 
	{
		while(data->width > 0) 
		{
			data->out(' ', data->ptr);
			data->idx++;
			data->width--;
		}
	}
	
  if (data->flags & FLAGS_NEGATIVE) 
	{
	  data->out('-', data->ptr);
	  data->idx++;
  }
  else if (data->flags & FLAGS_PLUS) 
	{
	  data->out('+', data->ptr);  // ignore the space if the '+' exists
	  data->idx++;
  }
  else if (data->flags & FLAGS_SPACE) 
	{
	  data->out(' ', data->ptr);
	  data->idx++;
  }

	// handle hash
  if (data->flags & FLAGS_HASH) 
	{
		data->out('0', data->ptr);
		data->idx++;
		if ((data->base == BASE_HEX) && !(data->flags & FLAGS_UPPERCASE)) 
		{
      data->out('x', data->ptr);
			data->idx++;
		}
    else if ((data->base == BASE_HEX) && (data->flags & FLAGS_UPPERCASE)) 
		{
      data->out('X', data->ptr);
			data->idx++;
		}
    else if ((data->base == BASE_BINARY)) 
		{
      data->out('b', data->ptr);
			data->idx++;
		}
  }

  // pad leading zeros
  if (!(data->flags & FLAGS_LEFT)) 
	{
	  while(data->precision > 0) 
		{
		  data->out('0', data->ptr);
		  data->idx++;
		  data->precision--;
	  }
	  while ((data->flags & FLAGS_ZEROPAD) && data->width > 0) 
		{
		  data->out('0', data->ptr);
		  data->idx++;
		  data->width--;
	  }
  }
}

static uint8_t get_digits_long(unsigned long value, base_t base)
{
	uint8_t digits = 0;
	if (base == BASE_DECIMAL)
	{
		while (digits < ARRAY_SIZE(pow10_long) && value >= pow10_long[digits])
			digits++;
	}
	else
	{
		while (value > 0)
		{
			digits++;
			value >>= base_bits[base];
		}
	}
	return digits;
}

//Silly function that decreases stack usage on IAR ARM compiler.
static void do_out(data_t *data, char c)
{
	data->out(c, data->ptr);
  data->idx++;
}

// internal itoa for 'long' type
static void ntoa_long(data_t *data, unsigned long value)
{
	// write if precision != 0 and value is != 0
	if (!(data->flags & FLAGS_PRECISION) || value) 
	{
		if (value == 0)
		{
			data->out('0', data->ptr);
			data->idx++;
		}
		else if(data->base == BASE_DECIMAL)
		{
			while(data->digit_count > 0)
			{
				data->digit_count--;
				unsigned long l = pow10_long[data->digit_count];
				uint8_t digit = value / l;
				value %= l;
				data->out(digit + '0', data->ptr);
				data->idx++;
			}
		}
		else
		{
			for(; data->digit_count > 0; data->digit_count--)
			{
				uint8_t digit = (value >> (data->digit_count-1) * base_bits[data->base]) & base_mask[data->base];
				data->out(digit < 10 ? '0' + digit : (data->flags & FLAGS_UPPERCASE ? 'A' : 'a') + digit - 10, data->ptr);
				data->idx++;
			}
		}
	}

	// append pad spaces up to given width
	if (data->flags & FLAGS_LEFT) 
	{
		while (data->width > 0) 
		{
			data->out(' ', data->ptr);
			data->width--;
		}
	}
}


// internal itoa for 'long long' type
#if defined(PRINTF_SUPPORT_LONG_LONG)
static uint8_t get_digits_long_long(unsigned long long value, base_t base)
{
	uint8_t digits = 0;
	if (base == BASE_DECIMAL)
	{
		while (digits < ARRAY_SIZE(pow10_long_long) && value >= pow10_long_long[digits])
			digits++;
	}
	else
	{
		while (value > 0)
		{
			digits++;
			value >>= base_bits[base];
		}
	}
	return digits;
}

// internal itoa for 'long' type
static void ntoa_long_long(data_t *data, unsigned long long value)
{
	// write if precision != 0 and value is != 0
	if (!(data->flags & FLAGS_PRECISION) || value) {
		if (value == 0)
		{
			data->out('0', data->ptr);
			data->idx++;
		}
		else if (data->base == BASE_DECIMAL)
		{
			while(data->digit_count > 0)
			{
				data->digit_count--;
				unsigned long long l = pow10_long_long[data->digit_count];
				uint8_t digit = value / l;
				value %= l;
				data->out(digit + '0', data->ptr);
				data->idx++;
			}
		}
		else
		{
			static const uint8_t bits[] = {1, 3, 0, 4};
			static const uint8_t mask[] = {1, 7, 0, 15};
			
			for(; data->digit_count > 0; data->digit_count--)
			{
				uint8_t digit = (value >> (data->digit_count-1)*bits[data->base]) & mask[data->base];
				data->out(digit < 10 ? '0' + digit : (data->flags & FLAGS_UPPERCASE ? 'A' : 'a') + digit - 10, data->ptr);
				data->idx++;
			}
		}
	}

	// append pad spaces up to given width
	if (data->flags & FLAGS_LEFT) {
		while (data->width > 0) {
			data->out(' ', data->ptr);
			data->width--;
		}
	}
}

static inline void pre_format_long_long(data_t *data, unsigned long long value, bool negative)
{
	data->digit_count = get_digits_long_long(value, data->base);
	// no hash for 0 values
	if (!value)
	{
		data->flags &= ~FLAGS_HASH;
	}
	if (negative)
	{
		data->flags |= FLAGS_NEGATIVE;
	}
}
#endif  // PRINTF_SUPPORT_LONG_LONG

static void print_string(data_t *data, const char *p)
{
	unsigned int l = strnlen_s(p, data->precision ? data->precision : (size_t)-1);
	// pre padding
	if (data->flags & FLAGS_PRECISION)
	{
		l = (l < data->precision ? l : data->precision);
}
	if (!(data->flags & FLAGS_LEFT))
	{
		while (l++ < data->width)
		{
			data->out(' ', data->ptr);
			data->idx++;
		}
	}
	// string output
	while ((*p != 0) && (!(data->flags & FLAGS_PRECISION) || data->precision--))
	{
		data->out(*(p++), data->ptr);
		data->idx++;
	}
	// post padding
	if (data->flags & FLAGS_LEFT)
	{
		while (l++ < data->width)
		{
			data->out(' ', data->ptr);
			data->idx++;
		}
	}
}

static inline void pre_format_long(data_t *data, unsigned long value, bool negative)
{
	data->digit_count = get_digits_long(value, data->base);
	// no hash for 0 values
	if (!value) 
	{
		data->flags &= ~FLAGS_HASH;
	}
	if(negative)
	{
		data->flags |= FLAGS_NEGATIVE;
	}
}

uint8_t format_out(out_fct_type out, void* ptr, const char* format, va_list va)
{
  unsigned int n;
	data_t data;
	//Assignment uses less stack than initialization with IAR ARM compiler!!!!
	data.out = out;
	data.ptr = ptr;
	data.idx = 0;

  while (*format)
  {
    // format specifier?  %[flags][width][.precision][length]
    if (*format != '%') 
		{
			do_out(&data, *format);
      format++;
      continue;
    }
    else 
		{
      // yes, evaluate it
      format++;
    }

    // evaluate flags
    data.flags = 0;
    do 
		{
      switch (*format) 
			{
        case '0': data.flags |= FLAGS_ZEROPAD; format++; n = 1U; break;
        case '-': data.flags |= FLAGS_LEFT;    format++; n = 1U; break;
        case '+': data.flags |= FLAGS_PLUS;    format++; n = 1U; break;
        case ' ': data.flags |= FLAGS_SPACE;   format++; n = 1U; break;
        case '#': data.flags |= FLAGS_HASH;    format++; n = 1U; break;
        default :                                   		 n = 0U; break;
      }
    } while (n);

    // evaluate width field
    data.width = 0U;
    if (is_digit(*format)) 
		{
      data.width = atoi(&format);
    }
    else if (*format == '*') 
		{
      const int w = va_arg(va, int);
      if (w < 0) 
			{
        data.flags |= FLAGS_LEFT;    // reverse padding
        data.width = (unsigned int)-w;
      }
      else 
			{
        data.width = (unsigned int)w;
      }
      format++;
    }

    // evaluate precision field
    data.precision = 0U;
    if (*format == '.') 
		{
      data.flags |= FLAGS_PRECISION;
      format++;
      if (is_digit(*format)) 
			{
        data.precision = atoi(&format);
      }
      else if (*format == '*') 
			{
        const int prec = (int)va_arg(va, int);
        data.precision = prec > 0 ? (unsigned int)prec : 0U;
        format++;
      }
    }

    // evaluate length field
    switch (*format) 
		{
      case 'l':
        data.flags |= FLAGS_LONG;
        format++;
        if (*format == 'l') 
				{
          data.flags |= FLAGS_LONG_LONG;
          format++;
        }
        break;
				
      case 'h':
        data.flags |= FLAGS_SHORT;
        format++;
        if (*format == 'h') 
				{
          data.flags |= FLAGS_CHAR;
          format++;
        }
        break;
				
#if defined(PRINTF_SUPPORT_PTRDIFF_T)
      case 't':
        data.flags |= (sizeof(ptrdiff_t) == sizeof(long) ? FLAGS_LONG : FLAGS_LONG_LONG);
        format++;
        break;
#endif
      case 'j':
        data.flags |= (sizeof(intmax_t) == sizeof(long) ? FLAGS_LONG : FLAGS_LONG_LONG);
        format++;
        break;
				
      case 'z':
        data.flags |= (sizeof(size_t) == sizeof(long) ? FLAGS_LONG : FLAGS_LONG_LONG);
        format++;
        break;
				
      default:
        break;
    }

    // evaluate specifier
    switch (*format) 
		{
      case 'd':
      case 'i':
      case 'u':
      case 'x':
      case 'X':
      case 'o':
      case 'b': 
        // set the base
        if (*format == 'x' || *format == 'X') 
				{
          data.base = BASE_HEX;
        }
        else if (*format == 'o') 
				{
          data.base =  BASE_OCTAL;
        }
        else if (*format == 'b') 
				{
          data.base =  BASE_BINARY;
        }
        else 
				{
          data.base = BASE_DECIMAL;
          data.flags &= ~FLAGS_HASH;   // no hash for dec format
        }
        // uppercase
        if (*format == 'X') 
				{
          data.flags |= FLAGS_UPPERCASE;
        }

        // no plus or space flag for u, x, X, o, b
        if ((*format != 'i') && (*format != 'd')) 
				{
          data.flags &= ~(FLAGS_PLUS | FLAGS_SPACE);
        }

        // ignore '0' flag when precision is given
        if (data.flags & FLAGS_PRECISION) 
				{
          data.flags &= ~FLAGS_ZEROPAD;
        }

        // convert the integer
        if ((*format == 'i') || (*format == 'd')) 
				{
          // signed
          if (data.flags & FLAGS_LONG_LONG) 
					{
#if defined(PRINTF_SUPPORT_LONG_LONG)
						const long long value = va_arg(va, long long);
						pre_format_long_long(&data, ABS(value), value < 0);
						ntoa_format(&data);
            ntoa_long_long(&data, ABS(value));
#endif
          }
          else if (data.flags & FLAGS_LONG) 
					{
            const long value = va_arg(va, long);
						pre_format_long(&data, ABS(value), value < 0);
						ntoa_format(&data);
            ntoa_long(&data, ABS(value));
          }
          else 
					{
            const int value = (data.flags & FLAGS_CHAR) ? (char)va_arg(va, int) : (data.flags & FLAGS_SHORT) ? (short int)va_arg(va, int) : va_arg(va, int);
						pre_format_long(&data, ABS(value), value < 0);
						ntoa_format(&data);
            ntoa_long(&data, ABS(value));
          }
        }
        else 
				{
          // unsigned
          if (data.flags & FLAGS_LONG_LONG) 
					{
#if defined(PRINTF_SUPPORT_LONG_LONG)
						unsigned long long value = va_arg(va, unsigned long long);
						pre_format_long_long(&data, value, false);
						ntoa_format(&data);
            ntoa_long_long(&data, value);
#endif
          }
          else if (data.flags & FLAGS_LONG) 
					{
						unsigned long value = va_arg(va, unsigned long);
						pre_format_long(&data, value, false);
						ntoa_format(&data);
            ntoa_long(&data, value);
          }
          else 
					{
            const unsigned int value = (data.flags & FLAGS_CHAR) ? (unsigned char)va_arg(va, unsigned int) : (data.flags & FLAGS_SHORT) ? (unsigned short int)va_arg(va, unsigned int) : va_arg(va, unsigned int);
						pre_format_long(&data, value, false);
						ntoa_format(&data);
            ntoa_long(&data, value);
          }
        }
        format++;
        break;

			case 'c': 
			{
        unsigned int l = 1U;
        // pre padding
        if (!(data.flags & FLAGS_LEFT)) 
				{
          while (l++ < data.width) 
					{
						do_out(&data, ' ');
		  		}
        }
        // char output
				do_out(&data, (char)va_arg(va, int));
				// post padding
        if (data.flags & FLAGS_LEFT) 
				{
          while (l++ < data.width) 
					{
						do_out(&data, ' ');
          }
        }
        format++;
        break;
      }

      case 's': 
				print_string(&data, va_arg(va, char*));
        format++;
        break;      

      case 'p': 
			{
        data.width = sizeof(void*) * 2U;
        data.flags |= FLAGS_ZEROPAD | FLAGS_UPPERCASE;
				data.base = BASE_HEX;
#if defined(PRINTF_SUPPORT_LONG_LONG)
        const bool is_ll = sizeof(uintptr_t) == sizeof(long long);
        if (is_ll) 
				{
					uintptr_t value = (uintptr_t)va_arg(va, void*);
					pre_format_long_long(&data, value, false);
					ntoa_format(&data);
          ntoa_long_long(&data, value);
        }
        else 
				{
#endif
					unsigned long value = (unsigned long)((uintptr_t)va_arg(va, void*));
					pre_format_long(&data, value, false);
					ntoa_format(&data);
          ntoa_long(&data, value);
#if defined(PRINTF_SUPPORT_LONG_LONG)
        }
#endif
        format++;
        break;
      }

      case '%':
				do_out(&data, '%');
				format++;
        break;

      default:
				do_out(&data, *format);
				format++;
        break;
    }
  }
    
  // return written chars without terminating \0
  return data.idx;
}


///////////////////////////////////////////////////////////////////////////////

int printf_(const char* format, ...)
{
  va_list va;
  va_start(va, format);
  const int ret = format_out(out_char, NULL, format, va);
  va_end(va);
  return ret;
}


int sprintf_(char* buffer, const char* format, ...)
{
	sprintf_data_t data = { buffer, -1 };
	va_list va;
  va_start(va, format);
  const int ret = format_out(out_sprintf, &data, format, va);
  va_end(va);
  *data.buffer = 0;
  return ret;
}


int snprintf_(char* buffer, size_t count, const char* format, ...)
{
	sprintf_data_t data = { buffer, count };
	va_list va;
  va_start(va, format);
  const int ret = format_out(out_sprintf, &data, format, va);
  va_end(va);
  if(buffer != NULL && count > 0)
		data.buffer[data.maxlen ? 0 : -1] = 0;
  return ret;
}


int vprintf_(const char* format, va_list va)
{
  return format_out(out_char, NULL, format, va);
}


int vsnprintf_(char* buffer, size_t count, const char* format, va_list va)
{
	sprintf_data_t data = { buffer, count };
  int ret = format_out(out_sprintf, &data, format, va);
  if (buffer != NULL && count > 0)
		data.buffer[data.maxlen ? 0 : -1] = 0;
  return ret;
}
