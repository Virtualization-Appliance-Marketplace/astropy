// Licensed under a 3-clause BSD style license - see LICENSE.rst

#include "tokenizer.h"

tokenizer_t *create_tokenizer(char delimiter, char comment, char quotechar, int fill_extra_cols)
{
    tokenizer_t *tokenizer = (tokenizer_t *) malloc(sizeof(tokenizer_t));
    tokenizer->source = 0;
    tokenizer->source_len = 0;
    tokenizer->source_pos = 0;
    tokenizer->delimiter = delimiter;
    tokenizer->comment = comment;
    tokenizer->quotechar = quotechar;
    tokenizer->header_output = 0;
    tokenizer->output_cols = 0;
    tokenizer->col_ptrs = 0;
    tokenizer->header_len = 0;
    tokenizer->output_len = 0;
    tokenizer->num_cols = 0;
    tokenizer->num_rows = 0;
    tokenizer->fill_extra_cols = fill_extra_cols;
    tokenizer->state = START_LINE;
    tokenizer->code = NO_ERROR;
    tokenizer->iter_col = 0;
    tokenizer->curr_pos = 0;
    tokenizer->buf = calloc(2, sizeof(char)); // This is a bit of a hack for empty field values
    return tokenizer;
}

void delete_data(tokenizer_t *tokenizer)
{
    // Don't free tokenizer->source because it points to part of an already freed Python object
    free(tokenizer->header_output);
    int i;
    if (tokenizer->output_cols)
	for (i = 0; i < tokenizer->num_cols; ++i)
	    free(tokenizer->output_cols[i]);
    
    free(tokenizer->output_cols);
    free(tokenizer->col_ptrs);
    tokenizer->header_output = 0;
    tokenizer->output_cols = 0;
    tokenizer->col_ptrs = 0;
}

void delete_tokenizer(tokenizer_t *tokenizer)
{
    delete_data(tokenizer);
    free(tokenizer->buf);
    free(tokenizer);
}

void resize_col(tokenizer_t *self, int index)
{
    int diff = self->col_ptrs[index] - self->output_cols[index];
    self->output_cols[index] = (char *) realloc(self->output_cols[index], 2 * self->output_len[index] * sizeof(char));
    memset(self->output_cols[index] + self->output_len[index] * sizeof(char), 0, self->output_len[index] * sizeof(char));
    self->output_len[index] *= 2;
    self->col_ptrs[index] = self->output_cols[index] + diff;
}

void resize_header(tokenizer_t *self)
{
    self->header_output = (char *) realloc(self->header_output, 2 * self->header_len * sizeof(char));
    memset(self->header_output + self->header_len * sizeof(char), 0, self->header_len * sizeof(char));
    self->header_len *= 2;
}

#define PUSH(c)								\
    if (header)								\
    {									\
        if (output_pos >= self->header_len)				\
        {								\
	    resize_header(self);					\
	}								\
	self->header_output[output_pos++] = c;				\
    }									\
    else if (col < self->num_cols && use_cols[real_col])		\
    {									\
	if (self->col_ptrs[col] - self->output_cols[col] >= self->output_len[col] * sizeof(char)) \
	{								\
	    resize_col(self, col);					\
	}								\
        *self->col_ptrs[col]++ = c;					\
    }

#define END_FIELD()							\
    if (header || use_cols[real_col])					\
    {									\
	PUSH('\x00');							\
	if (!header && ++col > self->num_cols)				\
	    RETURN(TOO_MANY_COLS);					\
    }									\
    ++real_col;

#define END_LINE()					\
    if (header)						\
	done = 1;					\
    else if (self->fill_extra_cols)			\
	while (col < self->num_cols)			\
	{						\
            PUSH('\x01');				\
	    END_FIELD();				\
	}						\
    else if (col < self->num_cols)			\
	RETURN(NOT_ENOUGH_COLS);			\
    ++self->num_rows;					\
    if (end != -1 && self->num_rows == end - start)	\
	done = 1;
    
#define RETURN(c) { self->code = c; return c; }

int tokenize(tokenizer_t *self, int start, int end, int header, int *use_cols)
{
    delete_data(self); // clear old reading data
    char c; // input character
    int col = 0; // current column ignoring possibly excluded columns
    int real_col = 0; // current column taking excluded columns into account
    int output_pos = 0; // current position in header output string
    self->header_len = INITIAL_HEADER_SIZE;
    self->source_pos = 0;
    self->num_rows = 0;
    int i = 0;
    int empty = 1;
    int comment = 0;
    
    //TODO: different error for no data
    //TODO: decide what to do about whitespace delimiter here
    while (i < start)
    {
	if (self->source_pos >= self->source_len - 1) // ignore final newline
	    RETURN(INVALID_LINE);
	if (self->source[self->source_pos] != '\n' && empty)
	{
	    empty = 0;
	    if (self->source[self->source_pos] == self->comment)
		comment = 1;
	}
	else if (self->source[self->source_pos++] == '\n')
	{
	    if (!empty && !comment)
		++i;
	    empty = 1;
	    comment = 0;
	}
    }
    
    if (header)
	self->header_output = (char *) calloc(1, INITIAL_HEADER_SIZE * sizeof(char));
    else
    {
	self->output_cols = (char **) malloc(self->num_cols * sizeof(char *));
	self->col_ptrs = (char **) malloc(self->num_cols * sizeof(char *));
	self->output_len = (int *) malloc(self->num_cols * sizeof(int));
	
	for (i = 0; i < self->num_cols; ++i)
	{
	    self->output_cols[i] = (char *) calloc(1, INITIAL_COL_SIZE * sizeof(char));
	    self->col_ptrs[i] = self->output_cols[i];
	    self->output_len[i] = INITIAL_COL_SIZE;
	}
    }
    
    int done = (end != -1 && end <= start);
    int repeat;
    self->state = START_LINE;
    
    while (self->source_pos < self->source_len && !done)
    {
	c = self->source[self->source_pos];
	repeat = 1;
	
	while (repeat && !done)
	{
	    repeat = 0;

	    switch (self->state)
	    {
	    case START_LINE:
		// TODO: make an option not to strip whitespace (for tab-delimited, etc.)
		if (c == '\n' || c == ' ' || c == '\t')
		    break;
		else if (c == self->comment)
		{
		    self->state = COMMENT;
		    break;
		}
		col = 0;
		real_col = 0;
		self->state = START_FIELD;
		repeat = 1;
		break;
		
	    case START_FIELD:
		if (c == ' ' || c == '\t') // TODO: strip whitespace at the end of fields as well
		    ;
		else if (c == self->delimiter)
		{
		    PUSH('\x01'); // indicates empty field
		    END_FIELD();
		}
		else if (c == '\n')
		{
		    END_LINE();
		    self->state = START_LINE;
		}
		else if (c == self->quotechar)
		    self->state = START_QUOTED_FIELD;
		else
		{
		    repeat = 1;
		    self->state = FIELD;
		}
		break;
		
	    case START_QUOTED_FIELD:
		if (c == ' ' || c == '\t')
		    ;
		else if (c == self->quotechar)
		{
		    PUSH('\x01'); // indicates empty field
		    END_FIELD();
		}
		else
		{
		    self->state = QUOTED_FIELD;
		    repeat = 1;
		}
		break;
		
	    case FIELD:
		if (c == self->delimiter)
		{
		    END_FIELD();
		    self->state = START_FIELD;
		}
		else if (c == '\n')
		{
		    END_FIELD();
		    END_LINE();
		    self->state = START_LINE;
		}
		else
		{
		    PUSH(c);
		}
		break;
		
	    case QUOTED_FIELD:
		if (c == self->quotechar)
		    self->state = FIELD;
		else if (c == '\n')
		    self->state = QUOTED_FIELD_NEWLINE;
		else
		{
		    PUSH(c);
		}
		break;

	    case QUOTED_FIELD_NEWLINE:
		if (c == ' ' || c == '\t' || c == '\n')
		    ;
		else if (c == self->quotechar)
		    self->state = FIELD; // TODO: fix this for empty data
		else
		{
		    repeat = 1;
		    self->state = QUOTED_FIELD;
		}
		break;
		
	    case COMMENT:
		if (c == '\n')
		    self->state = START_LINE;
		break;
	    }
	}
	
	++self->source_pos;
    }
    
    RETURN(0);
}

int int_size(void)
{
    return 8 * sizeof(int);
}

int str_to_int(tokenizer_t *self, char *str)
{
    errno = 0;
    char *tmp;
    int ret = strtol(str, &tmp, 0); // TODO: see if there are problems with long->int conversion

    if (tmp == str || *tmp != '\0' || errno == ERANGE)
 	self->code = CONVERSION_ERROR;

    return ret;
}

float str_to_float(tokenizer_t *self, char *str)
{
    char *tmp;
    float ret = strtod(str, &tmp); // TODO: same here with double->float

    if (ret == 0.0f && tmp == str)
	self->code = CONVERSION_ERROR;

    return ret;
}

void start_iteration(tokenizer_t *self, int col)
{
    self->iter_col = col;
    self->curr_pos = self->output_cols[col];
}

int finished_iteration(tokenizer_t *self)
{
    return (self->curr_pos - self->output_cols[self->iter_col] >= self->output_len[self->iter_col] * sizeof(char)
	    || *self->curr_pos == '\x00');
}

char *next_field(tokenizer_t *self)
{
    char *tmp = self->curr_pos;

    while (*self->curr_pos != '\x00') // pass through the entire field until reaching the delimiter
	++self->curr_pos;

    ++self->curr_pos; // next field begins after the delimiter
    if (*tmp == '\x01') // empty field; this is a hack
	return self->buf;
    else
	return tmp;
}
