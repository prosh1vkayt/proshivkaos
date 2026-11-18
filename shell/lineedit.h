/* shell/lineedit.h — ввод строки с историей и автодополнением по Tab. */
#ifndef SHELL_LINEEDIT_H
#define SHELL_LINEEDIT_H

#include <stddef.h>

/* Читает строку в buf (не больше max-1 символов, с завершающим нулём) и
 * возвращает её длину.
 *
 *   prompt  — печатает приглашение; зовётся повторно, когда после списка
 *             вариантов Tab нужно перерисовать строку.
 *   cmds    — имена команд для дополнения первого слова.
 *
 * Клавиши: ↑/↓ — история, Tab — дополнение (команды и имена файлов RamFS
 * для cat/edit/touch), Backspace, Ctrl+U — стереть строку, Ctrl+C — отмена. */
size_t line_read(char *buf, size_t max, void (*prompt)(void),
                 const char *const *cmds, size_t ncmds);

#endif
