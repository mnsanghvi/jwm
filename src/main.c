/**
 * @file main.c
 * @author Joe Wingbermuehle
 * @date 2004-2006
 *
 * @brief The main entry point and related JWM functions.
 *
 */

#include "jwm.h"
#include "main.h"
#include "lex.h"
#include "parse.h"
#include "help.h"
#include "error.h"
#include "event.h"

#include "border.h"
#include "client.h"
#include "color.h"
#include "command.h"
#include "cursor.h"
#include "confirm.h"
#include "font.h"
#include "hint.h"
#include "group.h"
#include "key.h"
#include "icon.h"
#include "taskbar.h"
#include "tray.h"
#include "traybutton.h"
#include "popup.h"
#include "pager.h"
#include "swallow.h"
#include "screen.h"
#include "root.h"
#include "desktop.h"
#include "place.h"
#include "clock.h"
#include "dock.h"
#include "misc.h"
#include "background.h"
#include "settings.h"
#include "timing.h"
#include "grab.h"

Display *display = NULL;
Window rootWindow;
int rootWidth, rootHeight;
int rootDepth;
int rootScreen;
Colormap rootColormap;
Visual *rootVisual;
GC rootGC;
int colormapCount;
Window supportingWindow;
Atom managerSelection;

char shouldExit = 0;
char shouldRestart = 0;
char isRestarting = 0;
char initializing = 0;
char shouldReload = 0;

unsigned int currentDesktop = 0;

char *exitCommand = NULL;

XContext clientContext;
XContext frameContext;

#ifdef USE_SHAPE
char haveShape;
int shapeEvent;
#endif
#ifdef USE_XRENDER
char haveRender;
#endif

static const char *CONFIG_FILE = "/.jwmrc";

static void Initialize(void);
static void Startup(void);
static void Shutdown(void);
static void Destroy(void);

static void OpenConnection(void);
static void CloseConnection(void);
static Bool SelectionReleased(Display *d, XEvent *e, XPointer arg);
static void StartupConnection(void);
static void ShutdownConnection(void);
static void EventLoop(void);
static void HandleExit(int sig);
static void DoExit(int code);
static void SendRestart(void);
static void SendExit(void);
static void SendReload(void);
static void SendJWMMessage(const char *message);

static char *displayString = NULL;

char *configPath = NULL;

/** The main entry point. */
int main(int argc, char *argv[])
{
   char *temp;
   int x;
   enum {
      ACTION_RUN,
      ACTION_RESTART,
      ACTION_EXIT,
      ACTION_RELOAD,
      ACTION_PARSE
   } action;

   StartDebug();

   /* Get the name of the user's local configuration file. */
   temp = getenv("HOME");
   if(temp) {
      configPath = Allocate(strlen(temp) + strlen(CONFIG_FILE) + 1);
      strcpy(configPath, temp);
      strcat(configPath, CONFIG_FILE);
   } else {
      configPath = CopyString(CONFIG_FILE);
   }

   /* Parse command line options. */
   action = ACTION_RUN;
   for(x = 1; x < argc; x++) {
      if(!strcmp(argv[x], "-v")) {
         DisplayAbout();
         DoExit(0);
      } else if(!strcmp(argv[x], "-h")) {
         DisplayHelp();
         DoExit(0);
      } else if(!strcmp(argv[x], "-p")) {
         action = ACTION_PARSE;
      } else if(!strcmp(argv[x], "-restart")) {
         action = ACTION_RESTART;
      } else if(!strcmp(argv[x], "-exit")) {
         action = ACTION_EXIT;
      } else if(!strcmp(argv[x], "-reload")) {
         action = ACTION_RELOAD;
      } else if(!strcmp(argv[x], "-display") && x + 1 < argc) {
         displayString = argv[++x];
      } else if(!strcmp(argv[x], "-f") && x + 1 < argc) {
         Release(configPath);
         configPath = CopyString(argv[++x]);
      } else {
         printf("unrecognized option: %s\n", argv[x]);
         DisplayHelp();
         DoExit(1);
      }
   }

   switch(action) {
   case ACTION_PARSE:
      Initialize();
      ParseConfig(configPath);
      DoExit(0);
   case ACTION_RESTART:
      SendRestart();
      DoExit(0);
   case ACTION_EXIT:
      SendExit();
      DoExit(0);
   case ACTION_RELOAD:
      SendReload();
      DoExit(0);
   default:
      break;
   }

#if defined(HAVE_SETLOCALE) && defined(ENABLE_NLS)
   setlocale(LC_ALL, "");
#endif
#ifdef HAVE_GETTEXT
   bindtextdomain("jwm", LOCALEDIR);
   textdomain("jwm");
#endif

   /* The main loop. */
   StartupConnection();
   do {

      isRestarting = shouldRestart;
      shouldExit = 0;
      shouldRestart = 0;
      shouldReload = 0;

      /* Prepare JWM components. */
      Initialize();

      /* Parse the configuration file. */
      ParseConfig(configPath);

      /* Start up the JWM components. */
      Startup();

      /* The main event loop. */
      EventLoop();

      /* Shutdown JWM components. */
      Shutdown();

      /* Perform any extra cleanup. */
      Destroy();

   } while(shouldRestart);
   ShutdownConnection();

   /* If we have a command to execute on shutdown, run it now. */
   if(exitCommand) {
      execl(SHELL_NAME, SHELL_NAME, "-c", exitCommand, NULL);
      Warning(_("exec failed: (%s) %s"), SHELL_NAME, exitCommand);
      DoExit(1);
   } else {
      DoExit(0);
   }

   /* Control shoud never get here. */
   return -1;

}

/** Exit with the specified status code. */
void DoExit(int code)
{

   Destroy();

   if(configPath) {
      Release(configPath);
      configPath = NULL;
   }
   if(exitCommand) {
      Release(exitCommand);
      exitCommand = NULL;
   }

   StopDebug();
   exit(code);
}

/** Main JWM event loop. */
void EventLoop(void)
{

   XEvent event;
   TimeType start;

   /* Loop processing events until it's time to exit. */
   while(JLIKELY(!shouldExit)) {
      if(JLIKELY(WaitForEvent(&event))) {
         ProcessEvent(&event);
      }
   }

   /* Process events one last time. */
   GetCurrentTime(&start);
   for(;;) {
      if(JXPending(display) == 0) {
         if(!IsSwallowPending()) {
            break;
         } else {
            TimeType now;
            GetCurrentTime(&now);
            if(GetTimeDifference(&start, &now) > RESTART_DELAY) {
               break;
            }
         }
      }
      if(WaitForEvent(&event)) {
         ProcessEvent(&event);
      }
   }

}

/** Open a connection to the X server. */
void OpenConnection(void)
{

   display = JXOpenDisplay(displayString);
   if(JUNLIKELY(!display)) {
      if(displayString) {
         printf("error: could not open display %s\n", displayString);
      } else {
         printf("error: could not open display\n");
      }
      DoExit(1);
   }

   rootScreen = DefaultScreen(display);
   rootWindow = RootWindow(display, rootScreen);
   rootWidth = DisplayWidth(display, rootScreen);
   rootHeight = DisplayHeight(display, rootScreen);
   rootDepth = DefaultDepth(display, rootScreen);
   rootColormap = DefaultColormap(display, rootScreen);
   rootVisual = DefaultVisual(display, rootScreen);
   rootGC = DefaultGC(display, rootScreen);
   colormapCount = MaxCmapsOfScreen(ScreenOfDisplay(display, rootScreen));

   XSetGraphicsExposures(display, rootGC, False);


}

/** Predicate for XIfEvent to determine if we got the WM_Sn selection. */
Bool SelectionReleased(Display *d, XEvent *e, XPointer arg)
{
   if(e->type == DestroyNotify) {
      if(e->xdestroywindow.window == *(Window*)arg) {
         return True;
      }
   }
   return False;
}

/** Prepare the connection. */
void StartupConnection(void)
{

   XSetWindowAttributes attr;
#ifdef USE_SHAPE
   int shapeError;
#endif
#ifdef USE_XRENDER
   int renderEvent;
   int renderError;
#endif
   struct sigaction sa;
   char name[32];
   Window owner;
   XEvent event;

   initializing = 1;
   OpenConnection();

#if 0
   XSynchronize(display, True);
#endif

   /* Create the supporting window used to verify JWM is running. */
   supportingWindow = JXCreateSimpleWindow(display, rootWindow,
                                           0, 0, 1, 1, 0, 0, 0);

   /* Get the atom used for the window manager selection. */
   snprintf(name, 32, "WM_S%d", rootScreen);
   managerSelection = JXInternAtom(display, name, False);

   /* Get the current window manager and take the selection. */
   GrabServer();
   owner = JXGetSelectionOwner(display, managerSelection);
   if(owner != None) {
      JXSelectInput(display, owner, StructureNotifyMask);
   }
   JXSetSelectionOwner(display, managerSelection,
                       supportingWindow, CurrentTime);
   UngrabServer();

   /* Wait for the current selection owner to give up the selection. */
   if(owner != None) {
      /* Note that we need to wait for the current selection owner
       * to exit before we can expect to select SubstructureRedirectMask. */
      XIfEvent(display, &event, SelectionReleased, (XPointer)&owner);
      JXSync(display, False);
   }

   event.xclient.display = display;
   event.xclient.type = ClientMessage;
   event.xclient.window = rootWindow;
   event.xclient.message_type = JXInternAtom(display, "MANAGER", False);
   event.xclient.format = 32;
   event.xclient.data.l[0] = CurrentTime;
   event.xclient.data.l[1] = managerSelection;
   event.xclient.data.l[2] = supportingWindow;
   event.xclient.data.l[3] = 2;
   event.xclient.data.l[4] = 0;
   JXSendEvent(display, rootWindow, False, StructureNotifyMask, &event);
   JXSync(display, False);

   JXSetErrorHandler(ErrorHandler);

   clientContext = XUniqueContext();
   frameContext = XUniqueContext();

   /* Set the events we want for the root window.
    * Note that asking for SubstructureRedirect will fail
    * if another window manager is already running.
    */
   attr.event_mask
      = SubstructureRedirectMask
      | SubstructureNotifyMask
      | StructureNotifyMask
      | PropertyChangeMask
      | ColormapChangeMask
      | ButtonPressMask
      | ButtonReleaseMask
      | PointerMotionMask | PointerMotionHintMask;
   JXChangeWindowAttributes(display, rootWindow, CWEventMask, &attr);

   memset(&sa, 0, sizeof(sa));
   sa.sa_flags = 0;
   sa.sa_handler = HandleExit;
   sigaction(SIGTERM, &sa, NULL);
   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGHUP, &sa, NULL);

   sa.sa_flags = SA_NOCLDWAIT;
   sa.sa_handler = SIG_DFL;
   sigaction(SIGCHLD, &sa, NULL);

#ifdef USE_SHAPE
   haveShape = JXShapeQueryExtension(display, &shapeEvent, &shapeError);
   if (haveShape) {
      Debug("shape extension enabled");
   } else {
      Debug("shape extension disabled");
   }
#endif

#ifdef USE_XRENDER
   haveRender = JXRenderQueryExtension(display, &renderEvent, &renderError);
   if(haveRender) {
      Debug("render extension enabled");
   } else {
      Debug("render extension disabled");
   }
#endif

   initializing = 0;

}

/** Close the X server connection. */
void CloseConnection(void)
{
   JXFlush(display);
   JXCloseDisplay(display);
}

/** Close the X server connection. */
void ShutdownConnection(void)
{
   CloseConnection();
}

/** Signal handler. */
void HandleExit(int sig)
{
   shouldExit = 1;
}

/** Initialize data structures.
 * This is called before the X connection is opened.
 */
void Initialize(void)
{

   InitializeBackgrounds();
   InitializeBorders();
   InitializeClients();
   InitializeClock();
   InitializeColors();
   InitializeCommands();
   InitializeCursors();
   InitializeDesktops();
#ifndef DISABLE_CONFIRM
   InitializeDialogs();
#endif
   InitializeDock();
   InitializeFonts();
   InitializeGroups();
   InitializeHints();
   InitializeIcons();
   InitializeKeys();
   InitializePager();
   InitializePlacement();
   InitializePopup();
   InitializeRootMenu();
   InitializeScreens();
   InitializeSettings();
   InitializeSwallow();
   InitializeTaskBar();
   InitializeTray();
   InitializeTrayButtons();
}

/** Startup the various JWM components.
 * This is called after the X connection is opened.
 */
void Startup(void)
{

   /* This order is important. */

   /* First we grab the server to prevent clients from changing things
    * while we're still loading. */
   GrabServer();

   StartupSettings();
   StartupScreens();

   StartupGroups();
   StartupColors();
   StartupIcons();
   StartupBackgrounds();
   StartupFonts();
   StartupCursors();

   StartupPager();
   StartupClock();
   StartupTaskBar();
   StartupTrayButtons();
   StartupDock();
   StartupTray();
   StartupKeys();
   StartupDesktops();
   StartupHints();
   StartupBorders();
   StartupPlacement();
   StartupClients();

#  ifndef DISABLE_CONFIRM
      StartupDialogs();
#  endif
   StartupPopup();

   StartupRootMenu();

   SetDefaultCursor(rootWindow);
   ReadCurrentDesktop();
   JXFlush(display);

   RestackClients();

   /* Allow clients to do their thing. */
   JXSync(display, True);
   UngrabServer();

   StartupSwallow();

   DrawTray();

   /* Send expose events. */
   ExposeCurrentDesktop();

   /* Draw the background (if backgrounds are used). */
   LoadBackground(currentDesktop);

   /* Run any startup commands. */
   StartupCommands();

}

/** Shutdown the various JWM components.
 * This is called before the X connection is closed.
 */
void Shutdown(void)
{

   /* This order is important. */

   ShutdownSwallow();

#  ifndef DISABLE_CONFIRM
      ShutdownDialogs();
#  endif
   ShutdownPopup();
   ShutdownKeys();
   ShutdownPager();
   ShutdownRootMenu();
   ShutdownDock();
   ShutdownTray();
   ShutdownTrayButtons();
   ShutdownTaskBar();
   ShutdownClock();
   ShutdownBorders();
   ShutdownClients();
   ShutdownBackgrounds();
   ShutdownIcons();
   ShutdownCursors();
   ShutdownFonts();
   ShutdownColors();
   ShutdownGroups();
   ShutdownDesktops();

   ShutdownPlacement();
   ShutdownHints();
   ShutdownScreens();
   ShutdownSettings();

   ShutdownCommands();

}

/** Clean up memory.
 * This is called after the X connection is closed.
 * Note that it is possible for this to be called more than once.
 */
void Destroy(void)
{
   DestroyBackgrounds();
   DestroyBorders();
   DestroyClients();
   DestroyClock();
   DestroyColors();
   DestroyCommands();
   DestroyCursors();
   DestroyDesktops();
#ifndef DISABLE_CONFIRM
   DestroyDialogs();
#endif
   DestroyDock();
   DestroyFonts();
   DestroyGroups();
   DestroyHints();
   DestroyIcons();
   DestroyKeys();
   DestroyPager();
   DestroyPlacement();
   DestroyPopup();
   DestroyRootMenu();
   DestroyScreens();
   DestroySettings();
   DestroySwallow();
   DestroyTaskBar();
   DestroyTray();
   DestroyTrayButtons();
}

/** Send _JWM_RESTART to the root window. */
void SendRestart(void)
{
	SendJWMMessage(jwmRestart);
}

/** Send _JWM_EXIT to the root window. */
void SendExit(void)
{
	SendJWMMessage(jwmExit);
}

/** Send _JWM_RELOAD to the root window. */
void SendReload(void)
{
	SendJWMMessage(jwmReload);
}

/** Send a JWM message to the root window. */
void SendJWMMessage(const char *message)
{
   XEvent event;
   OpenConnection();
   memset(&event, 0, sizeof(event));
   event.xclient.type = ClientMessage;
   event.xclient.window = rootWindow;
   event.xclient.message_type = JXInternAtom(display, message, False);
   event.xclient.format = 32;
   JXSendEvent(display, rootWindow, False, SubstructureRedirectMask, &event);
   CloseConnection();
}

