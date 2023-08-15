/*-
 * Copyright (c) 2022 Devin Teske <dteske@FreeBSD.org>
 * Copyright (c) 2022 Faraz Vahedi <kfv@kfv.io>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _FIGPUT_H_
#define _FIGPUT_H_

#include <sys/types.h>

/*
 * Union for storing various types of data in a single common container.
 */
union figput_cfgvalue {
    void		*data;		/* Pointer to NUL-terminated string */
    char		*str;		/* Pointer to NUL-terminated string */
    char		**strarray;	/* Pointer to an array of strings */
    int32_t		num;		/* Signed 32-bit integer value */
    uint32_t	u_num;		/* Unsigned 32-bit integer value */
    uint32_t	boolean:1;	/* Boolean integer value (0 or 1) */
};

/*
 * Option types (based on above cfgvalue union)
 */
enum figput_cfgtype {
    FIGPUT_TYPE_NONE		= 0x0000, /* directives with no value */
    FIGPUT_TYPE_BOOL		= 0x0001, /* boolean */
    FIGPUT_TYPE_INT		= 0x0002, /* signed 32 bit integer */
    FIGPUT_TYPE_UINT		= 0x0004, /* unsigned 32 bit integer */
    FIGPUT_TYPE_STR		= 0x0008, /* string pointer */
    FIGPUT_TYPE_STRARRAY	= 0x0010, /* string array pointer */
    FIGPUT_TYPE_DATA1		= 0x0020, /* void data type-1 (open) */
    FIGPUT_TYPE_DATA2		= 0x0040, /* void data type-2 (open) */
    FIGPUT_TYPE_DATA3		= 0x0080, /* void data type-3 (open) */
    FIGPUT_TYPE_RESERVED1	= 0x0100, /* reserved data type-1 */
    FIGPUT_TYPE_RESERVED2	= 0x0200, /* reserved data type-2 */
    FIGPUT_TYPE_RESERVED3	= 0x0400, /* reserved data type-3 */
};

/*
 * Options to put_config() for put_options bitmask
 */
#define FIGPUT_NO_SAVE_DEFAULTS     0x0001 /* Error if new value == default */
#define FIGPUT_NO_SAVE_DUPLICATES   0x0002 /* Error if directive found twice */
#define FIGPUT_SAVE_ALLOW_EMPTY     0x0004 /* Directive can have no value if
					    * type is one of:
					    *   - FP_TYPE_NONE
					    *   - FP_TYPE_BOOL
					    * NB: for latter, default must be
					    * false, otherwise raise error
					    */
#define FIGPUT_SAVE_BACKUP          0x0008 /* Back up config file */
#define FIGPUT_SAVE_UNQUOTED        0x0010 /* Save values without quotes */

/*
 * Per-directive actions
 */
#define FIGPUT_ACTION_SET_VALUE     0x0000 /* Default */
#define FIGPUT_ACTION_CHECK         0x0001 /* Check current value */
#define FIGPUT_ACTION_REMOVE        0x0002 /* Remove directive from config */

/*
 * Per-directive result codes
 */
#define FIGPUT_DIRECTIVE_FOUND      0x0001 /* vs not found (see added) */
#define FIGPUT_VALUE_CHANGED        0x0002 /* vs no change required */
#define FIGPUT_DIRECTIVE_ADDED      0x0004 /* vs already existed */
#define FIGPUT_DIRECTIVE_REMOVED    0x0008 /* vs not found */

/*
 * Options to put_config() for processing_options bitmask
 */
#define FIGPUT_BREAK_ON_EQUALS    0x0001 /* stop reading directive at `=' */
#define FIGPUT_BREAK_ON_SEMICOLON 0x0002 /* `;' starts a new line */
#define FIGPUT_CASE_SENSITIVE     0x0004 /* directives are case sensitive */
#define FIGPUT_REQUIRE_EQUALS     0x0008 /* assignment directives only */
#define FIGPUT_STRICT_EQUALS      0x0010 /* `=' must be part of directive */

/*
 * Anatomy of a config file option for writing
 */
struct figput_config {
	enum figput_cfgtype	type;		/* Option value type */
	const char		*directive;	/* config file keyword */
	union figput_cfgvalue	value;		/* NB: for action to write */
	uint8_t			action;		/* Action to perform */
	uint16_t		result;		/* NB: set by put_config */
	uint32_t		line;		/* NB: set by put_config */

	/*
	 * Function pointer; how to write the directive
	 */
	int (*write)(struct figput_config *option);
};

__BEGIN_DECLS
int	put_config(struct figput_config _options[static 1], const char *_path,
	    uint16_t _processing_options, uint16_t _put_options);
int	set_config_option(struct figput_config _options[static 1],
	    const char *_directive, union figput_cfgvalue *_value);
__END_DECLS

#endif /* _FIGPUT_H_ */
