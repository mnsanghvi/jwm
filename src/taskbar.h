/**
 * @file taskbar.h
 * @author Joe Wingbermuehle
 * @date 2005-2006
 *
 * @brief Task list tray component.
 *
 */

#ifndef TASKBAR_H
#define TASKBAR_H

struct ClientNode;
struct TimeType;

/*@{*/
void InitializeTaskBar(void);
#define StartupTaskBar()   (void)(0)
void ShutdownTaskBar(void);
void DestroyTaskBar(void);
/*@}*/

/** Create a new task bar tray component.
 * @param border 1 if the buttons should have a border, 0 otherwise.
 */
struct TrayComponentType *CreateTaskBar(char border);

/** Add a client to the task bar(s).
 * @param np The client to add.
 */
void AddClientToTaskBar(struct ClientNode *np);

/** Remove a client from the task bar(s).
 * @param np The client to remove.
 */
void RemoveClientFromTaskBar(struct ClientNode *np);

/** Update all task bars. */
void UpdateTaskBar(void);

/** Focus the next client in the task bar. */
void FocusNext(void);

/** Focus the previous client in the task bar. */
void FocusPrevious(void);

/** Set the maximum width of task bar items.
 * @param cp The task bar component.
 * @param value The maximum width.
 */
void SetMaxTaskBarItemWidth(struct TrayComponentType *cp, const char *value);

/** Update the _NET_CLIENT_LIST property. */
void UpdateNetClientList(void);

#endif /* TASKBAR_H */

