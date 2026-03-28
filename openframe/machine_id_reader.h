#ifndef MACHINE_ID_READER_H
#define MACHINE_ID_READER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read machine ID from shared OpenFrame location
 * - macOS: /Library/Application Support/OpenFrame/machine_id
 * - Windows: C:\ProgramData\OpenFrame\machine_id
 * - Linux: /var/lib/openframe/machine_id
 *
 * @return Machine ID string (caller must free) or NULL if not found
 */
char* read_machine_id(void);

#ifdef __cplusplus
}
#endif

#endif // MACHINE_ID_READER_H
