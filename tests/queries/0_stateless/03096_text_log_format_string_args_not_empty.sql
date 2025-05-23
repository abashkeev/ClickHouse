set enable_analyzer = true;

select count; -- { serverError UNKNOWN_IDENTIFIER }

select conut(); -- { serverError UNKNOWN_FUNCTION }

system flush logs text_log;

SET max_rows_to_read = 0; -- system.text_log can be really big
select count() > 0 from system.text_log where message_format_string = '{}{} memory usage: {}.' and not empty(value1) and value3 like '% MiB';

select count() > 0 from system.text_log where level = 'Error' and message_format_string = 'Unknown {}{} identifier {} in scope {}{}' and value1 = 'expression' and value3 = '`count`' and value4 = 'SELECT count';

select count() > 0 from system.text_log where level = 'Error' and message_format_string = 'Function with name {} does not exist. In scope {}{}' and value1 = '`conut`' and value2 = 'SELECT conut()' and value3 ilike '%\'count\'%';
