/*
    Platypus - create MacOS X application bundles that execute scripts
        This is the executable that goes into Platypus apps
    Copyright (C) 2003 Sveinbjorn Thordarson <sveinbt@hi.is>

    With modifications by Aaron Voisine for gimp.app
    With modifications by Marianne gagnon for Wilber-loves-apple
    With modifications by Michael Wybrow for Inkscape.app
    With modifications by ~suv for Inkscape.app
    With modifications by Gerald Combs for Wireshark.app

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
    USA.

    main.c - main program file

*/

/*
 * This app laucher basically takes care of:
 * - launching Wireshark (and, on OS X prior to Leopard, X11) when
 *   double-clicked (it's auto-launched by launchd on Leopard and later)
 * - bringing X11 to the top when its icon is clicked in the dock (via a small applescript)
 * - catch file dropped on icon events (and double-clicked gimp documents) and notify gimp.
 * - catch quit events performed outside gimp, e.g. on the dock icon.
 */

///////////////////////////////////////
// Includes
///////////////////////////////////////
#pragma mark Includes

// Apple stuff

// Note: including Carbon prevents building the launcher app in x86_64
//       used for StandardAlert in RequestUserAttention(),
//       RedFatalAlert()
#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>
#ifdef MAC_OS_X_VERSION_10_6
// Not available when building for 10.5
// Yes, this is how you have to test for "building for something before
// 10.6" - you can't compare anything against MAC_OS_X_VERSION_10_6
// when building with the 10.5 SDK, because MAC_OS_X_VERSION_10_6 isn't
// defined in the 10.5 SDK
#include <ServiceManagement/ServiceManagement.h>
#endif

// Unix stuff
#include <sys/param.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/utsname.h>

///////////////////////////////////////
// Definitions
///////////////////////////////////////
#pragma mark Definitions

// name length limits
#define	kMaxPathLength 1024

// names of files bundled with app
#define	kScriptFileName "script"
#define kOpenDocFileName "openDoc"
#if MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_6
#define kXQuartzFixerFileName CFSTR("XQuartzFixer")
#endif

// custom carbon event class
#define kEventClassRedFatalAlert 911

// custom carbon event types
#define kEventKindX11Failed 911
#define kEventKindFCCacheFailed 912

//maximum arguments the script accepts
#define	kMaxArgumentsToScript 252

///////////////////////////////////////
// Prototypes
///////////////////////////////////////
#pragma mark Prototypes

static void *Execute(void *arg);
static void *OpenDoc(void *arg);
static OSErr ExecuteScript(char *script, pid_t *pid);

static void  GetParameters(void);
static unsigned char* GetScript(void);
static unsigned char* GetOpenDoc(void);
#if MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_6
static CFStringRef GetXQuartzFixer(void);
#endif

OSErr LoadMenuBar(char *appName);

static OSStatus FSMakePath(FSRef fileRef, unsigned char *path, long maxPathSize);
static void RedFatalAlert(Str255 errorString, Str255 expStr);
static short DoesFileExist(unsigned char *path);

static OSErr AppQuitAEHandler(const AppleEvent *theAppleEvent,
                              AppleEvent *reply, long refCon);
static OSErr AppOpenDocAEHandler(const AppleEvent *theAppleEvent,
                                 AppleEvent *reply, long refCon);
static OSErr AppOpenAppAEHandler(const AppleEvent *theAppleEvent,
                                 AppleEvent *reply, long refCon);
static OSStatus X11FailedHandler(EventHandlerCallRef theHandlerCall,
                                 EventRef theEvent, void *userData);
static OSStatus FCCacheFailedHandler(EventHandlerCallRef theHandlerCall,
                                 EventRef theEvent, void *userData);
static OSErr AppReopenAppAEHandler(const AppleEvent *theAppleEvent,
                                   AppleEvent *reply, long refCon);

#if MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_6
static int ShowFixXQuartzDialog(void);
static void ShowMustFixXQuartzDialog(void);
#endif
static void ShowMustInstallX11Dialog(void);

static OSStatus CompileAppleScript(const void* text, long textLength,
                                  AEDesc *resultData);
static OSStatus SimpleCompileAppleScript(const char* theScript);
static OSErr runScript();

///////////////////////////////////////
// Globals
///////////////////////////////////////
#pragma mark Globals

// process id of forked process
pid_t pid = 0;

// thread id of threads that start scripts
pthread_t odtid = 0, tid = 0;

// indicator of whether the script has completed executing
short taskDone = true;

// execution parameters
char scriptPath[kMaxPathLength];
char openDocPath[kMaxPathLength];

//arguments to the script
char *arguments[kMaxArgumentsToScript+3];
char *fileArgs[kMaxArgumentsToScript];
short numArgs = 0;

extern char **environ;

//
// 0 if we should expect a bundled X11, 1 if we should expect XQuartz
//
static int expect_xquartz;

static enum {
    BUNDLED_X11,       // bundled X11, in /usr/X11
    MISSING_X11,       // no /usr/X11, but it's offered with the OS install
    XQUARTZ,           // unbundled XQuartz, in /opt/X11, with /usr/X11 linking to it
    XQUARTZ_STUB,      // stub libraries in /usr/X11 that will tell user to install XQuartz
    MISSING_XQUARTZ,   // no /opt/X11 or /usr/X11, and it's not bundled
    FIXABLE_XQUARTZ,   // unbundled XQuartz, in /opt/X11, with no /usr/X11
    UNFIXABLE_XQUARTZ, // /opt/X11 and /usr/X11, but the latter isn't a symlink to /opt/X11
    NO_X11             // no X11
} x11_type;

#pragma mark -

///////////////////////////////////////
// Program entrance point
///////////////////////////////////////
int main(int argc, char* argv[])
{
    OSErr err = noErr;
    EventTypeSpec X11events = { kEventClassRedFatalAlert, kEventKindX11Failed };
    EventTypeSpec FCCacheEvents = { kEventClassRedFatalAlert, kEventKindFCCacheFailed };
    struct utsname os_info;
    struct stat statb_usr_x11, statb_opt_x11;
    char symlink_target[MAXPATHLEN+1];
    ssize_t link_length;

    // Which version of OS X is this?  Use that to determine what sort
    // of X11 we should expect to have installed
    if (uname(&os_info) == -1) {
        // Couldn't find out; this "should not happen",
        // assume bundled X11 for the lulz
        expect_xquartz = 0;
    } else {
        if (os_info.release[0] <= '9' && os_info.release[1] == '.') {
            // Darwin 9.0 (Leopard) or earlier; expect bundled X11
            expect_xquartz = 0;
        } else if (os_info.release[0] == '1' &&
                   (os_info.release[1] == '0' || os_info.release[1] == '1') &&
                   os_info.release[2] == '.') {
            // Darwin 10.0 (Snow Leopard) or 11.0 (Lion); expect bundled X11
            expect_xquartz = 0;
        } else {
            // Mountain Lion or later; expect XQuartz
            expect_xquartz = 1;
        }
    }

    if (expect_xquartz) {
        // Do we have /opt/X11?
        if (lstat("/opt/X11", &statb_opt_x11) == -1) {
            // No.  Is /usr/X11 a directory?
            if (lstat("/usr/X11", &statb_usr_x11) != -1 &&
                S_ISDIR(statb_opt_x11.st_mode)) {
                // It's a directory; assume it contains the stub libraries.
                x11_type = XQUARTZ_STUB;
            } else {
                // It's not a directory; assume we need XQuartz installed.
                x11_type = MISSING_XQUARTZ;
            }
        } else {
            // Yes.  Is /usr/X11 a symbolic link to /opt/X11?
            if (lstat("/usr/X11", &statb_usr_x11) != -1) {
                if (S_ISLNK(statb_opt_x11.st_mode)) {
                    // OK, it's a symlink; does it point to /opt/X11?
                    link_length = readlink("/usr/X11", symlink_target, MAXPATHLEN);
                    if (link_length == -1) {
                        // Couldn't read it; no obvious fix
                        x11_type = UNFIXABLE_XQUARTZ;
                    } else {
                        // Read it; nul-terminate the string
                        symlink_target[link_length] = '\0';
                        if (strcmp(symlink_target, "/opt/X11") == 0) {
                            // Yes, it points to /opt/X11, so that's good
                            x11_type = XQUARTZ;
                        } else {
                            // No, it doesn't - broken
                            x11_type = UNFIXABLE_XQUARTZ;
                        }
                    }
                } else {
                    // Not a symlink; no otvbious fix
                    x11_type = UNFIXABLE_XQUARTZ;
                }
            } else {
                // Non-existent
                x11_type = FIXABLE_XQUARTZ;
            }
        }
    } else {
        // Look for /usr/X11
        if (lstat("/usr/X11", &statb_usr_x11) == -1) {
            // No /usr/X11; tell the user to install X11
            x11_type = MISSING_X11;
        } else {
            // Assume it's OK
            x11_type = BUNDLED_X11;
        }
    }

    InitCursor();

    //install Apple Event handlers
    err += AEInstallEventHandler(kCoreEventClass, kAEQuitApplication,
                                 NewAEEventHandlerUPP(AppQuitAEHandler),
                                 0, false);
    err += AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments,
                                 NewAEEventHandlerUPP(AppOpenDocAEHandler),
                                 0, false);
    err += AEInstallEventHandler(kCoreEventClass, kAEOpenApplication,
                                 NewAEEventHandlerUPP(AppOpenAppAEHandler),
                                 0, false);

    err += AEInstallEventHandler(kCoreEventClass, kAEReopenApplication,
                                 NewAEEventHandlerUPP(AppReopenAppAEHandler),
                                 0, false);

    err += InstallEventHandler(GetApplicationEventTarget(),
                               NewEventHandlerUPP(X11FailedHandler), 1,
                               &X11events, NULL, NULL);
    err += InstallEventHandler(GetApplicationEventTarget(),
                               NewEventHandlerUPP(FCCacheFailedHandler), 1,
                               &FCCacheEvents, NULL, NULL);

    if (err) RedFatalAlert("\pInitialization Error",
                           "\pError initing Apple Event handlers.");

    //create the menu bar
    if (err = LoadMenuBar(NULL)) RedFatalAlert("\pInitialization Error",
                                               "\pError loading MenuBar.nib.");

    GetParameters(); //load data from files containing exec settings

    switch (x11_type) {

    case FIXABLE_XQUARTZ:
        //
        // Alas, even though my 10.5 installation has the ServiceManagement
        // framework, it's not part of the 10.5 SDK; maybe it was for
        // Apple use only in 10.5.
        //
        // This means that the 32-bit built-for-10.5-and-later version
        // of Wireshark won't repair XQuartz, but if you had a machine
        // that originally ran Leopard, and you've patiently upgraded
        // it from release to releaase, one at a time, up to a release
        // whose installer damages the XQuartz installation that a
        // previous upgrade required - *if* there's a machine on that
        // can run all those releases! - you probably should have upgraded
        // to a 64-bit version of Wireshark somewhere along the line.
        // There are probably so few of those people for it to be worth
        // trying to fix things for them.
        //
#if MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_6
        /*
         * We have an XQuartz installation with no /usr/X11; offer the user
         * the choice to repair it, by re-planting the /usr/X11 symlink.
         */
        if (ShowFixXQuartzDialog()) {
            CFStringRef job_label = CFSTR("org.wireshark.fixXQuartz");
            AuthorizationItem authItem = { kSMRightBlessPrivilegedHelper, 0, NULL, 0 };
            AuthorizationRights authRights = { 1, &authItem };
            AuthorizationFlags flags = kAuthorizationFlagInteractionAllowed | kAuthorizationFlagPreAuthorize | kAuthorizationFlagExtendRights;
            AuthorizationRef auth;
            const void *keys[3] = {
                CFSTR("Label"),
                CFSTR("RunAtLoad"),
                CFSTR("Program")
            };
            const void *values[3];
            CFDictionaryRef dict;
            CFErrorRef error;

            if (AuthorizationCreate(&authRights,
                kAuthorizationEmptyEnvironment, flags, &auth) == errAuthorizationSuccess) {
                (void) SMJobRemove(kSMDomainSystemLaunchd, job_label, auth, false, NULL);

                values[0] = job_label;
                values[1] = kCFBooleanTrue;
                values[2] = GetXQuartzFixer();
                dict = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
                    &kCFTypeDictionaryKeyCallBacks,
                    &kCFTypeDictionaryValueCallBacks);

                if (SMJobSubmit(kSMDomainSystemLaunchd, dict, auth, &error)) {
                    // Job ran, so XQuartz should be fixed
                    x11_type = XQUARTZ;
                } else {
                    // Fail
                }
                if (error) {
                    CFRelease( error );
                }

                (void) SMJobRemove(kSMDomainSystemLaunchd, job_label, auth, false, NULL);
                AuthorizationFree(auth, 0);
            }
        } else {
            // You won't be able to run Wireshark
            ShowMustFixXQuartzDialog();
            return 0;
        }
#endif
        break;

    case MISSING_X11:
        // You need to install X11 to run Wireshark
        ShowMustInstallX11Dialog();
        return 0;
        break;

    default:
        // Either it should work, or should prompt you to install XQuartz,
        // or the fix isn't obvious
        break;
    }

    // compile "icon clicked" script so it's ready to execute
    // Don't tell it to activate if it's not installed;
    // that will pop up the annoying "where is XQuartz?"/"where is X11?"
    // dialog, and if somebody selects the wrong app, you end up with
    // a messed-up system that fails to start X correctly.
    switch (x11_type) {
    case XQUARTZ:
        SimpleCompileAppleScript("tell application \"XQuartz\" to activate");
        break;

    case BUNDLED_X11:
        SimpleCompileAppleScript("tell application \"X11\" to activate");
        break;

    default:
        break;
    }

    RunApplicationEventLoop(); //Run the event loop
    return 0;
}

#pragma mark -


static void RequestUserAttention(void)
{
    NMRecPtr notificationRequest = (NMRecPtr) NewPtr(sizeof(NMRec));

    memset(notificationRequest, 0, sizeof(*notificationRequest));
    notificationRequest->qType = nmType;
    notificationRequest->nmMark = 1;
    notificationRequest->nmIcon = 0;
    notificationRequest->nmSound = 0;
    notificationRequest->nmStr = NULL;
    notificationRequest->nmResp = NULL;

    verify_noerr(NMInstall(notificationRequest));
}


static void ShowFirstStartWarningDialog(void)
{
    SInt16 itemHit;

    AlertStdAlertParamRec params;
    params.movable = true;
    params.helpButton = false;
    params.filterProc = NULL;
    params.defaultText = (void *) kAlertDefaultOKText;
    params.cancelText = NULL;
    params.otherText = NULL;
    params.defaultButton = kAlertStdAlertOKButton;
    params.cancelButton = kAlertStdAlertCancelButton;
    params.position = kWindowDefaultPosition;

    StandardAlert(kAlertNoteAlert, "\pWireshark on Mac OS X",
            "\pWhile Wireshark is open, its windows can be displayed or hidden by displaying or hiding the X11 application.\n\nThe first time this version of Wireshark is run it may take several minutes before the main window is displayed while font caches are built.",
            &params, &itemHit);
}

#if MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_6
static int ShowFixXQuartzDialog(void)
{
        SInt16 itemHit;
        AlertStdAlertParamRec params;

        params.movable = true;
        params.helpButton = false;
        params.filterProc = NULL;
        params.defaultText = "\pYes, please repair it";
        params.cancelText = "\pNo, don't repair it";
        params.otherText = NULL;
        params.defaultButton = kAlertStdAlertOKButton;
        params.cancelButton = kAlertStdAlertCancelButton;
        params.position = kWindowDefaultPosition;

        StandardAlert(kAlertNoteAlert, "\pYour XQuartz installation appears to be damaged.  Would you like it to be repaired?",
            "\pWireshark will not be able to work if it is not repaired.",
            &params, &itemHit);

	return (itemHit == kAlertStdAlertOKButton);
}

static void ShowMustFixXQuartzDialog(void)
{
    SInt16 itemHit;

    AlertStdAlertParamRec params;
    params.movable = true;
    params.helpButton = false;
    params.filterProc = NULL;
    params.defaultText = (void *) kAlertDefaultOKText;
    params.cancelText = NULL;
    params.otherText = NULL;
    params.defaultButton = kAlertStdAlertOKButton;
    params.cancelButton = kAlertStdAlertCancelButton;
    params.position = kWindowDefaultPosition;

    StandardAlert(kAlertNoteAlert, "\pWireshark won't run with a damaged XQuartz installation.",
            "\pIf you want to run Wireshark, you will have to fix the XQuartz installation.",
            &params, &itemHit);
}
#endif

static void ShowMustInstallX11Dialog(void)
{
    SInt16 itemHit;

    AlertStdAlertParamRec params;
    params.movable = true;
    params.helpButton = false;
    params.filterProc = NULL;
    params.defaultText = (void *) kAlertDefaultOKText;
    params.cancelText = NULL;
    params.otherText = NULL;
    params.defaultButton = kAlertStdAlertOKButton;
    params.cancelButton = kAlertStdAlertCancelButton;
    params.position = kWindowDefaultPosition;

    StandardAlert(kAlertNoteAlert, "\pYou must install X11 from your Mac OS X install disk.",
            "\pWireshark will not run without X11.",
            &params, &itemHit);
}


//////////////////////////////////
// Handler for when fontconfig caches need to be generated
//////////////////////////////////
static OSStatus FCCacheFailedHandler(EventHandlerCallRef theHandlerCall,
                                 EventRef theEvent, void *userData)
{

    pthread_join(tid, NULL);
    if (odtid) pthread_join(odtid, NULL);

    // Bounce Wireshark Dock icon
    RequestUserAttention();
    // Need to show warning to the user, then carry on.
    ShowFirstStartWarningDialog();

    // Note that we've seen the warning.
    system("test -d \"$HOME/.wireshark\" || mkdir \"$HOME/.wireshark\"; "
           "touch \"$HOME/.wireshark/.fccache-new\"");
    // Rerun now.
    OSErr err = ExecuteScript(scriptPath, &pid);
    ExitToShell();

    return noErr;
}

///////////////////////////////////
// Execution thread starts here
///////////////////////////////////
static void *Execute (void *arg)
{
    EventRef event;

    taskDone = false;

    OSErr err = ExecuteScript(scriptPath, &pid);
    if (err == (OSErr)11) {
        CreateEvent(NULL, kEventClassRedFatalAlert, kEventKindX11Failed, 0,
                    kEventAttributeNone, &event);
        PostEventToQueue(GetMainEventQueue(), event, kEventPriorityStandard);
    }
    else if (err == (OSErr)12) {
        CreateEvent(NULL, kEventClassRedFatalAlert, kEventKindFCCacheFailed, 0,
                    kEventAttributeNone, &event);
        PostEventToQueue(GetMainEventQueue(), event, kEventPriorityHigh);
    }
    else ExitToShell();
    return 0;
}

///////////////////////////////////
// Open additional documents thread starts here
///////////////////////////////////
static void *OpenDoc (void *arg)
{
    ExecuteScript(openDocPath, NULL);
    return 0;
}

///////////////////////////////////////
// Run a script via the system command
///////////////////////////////////////
static OSErr ExecuteScript (char *script, pid_t *pid)
{
    pid_t wpid = 0, p = 0;
    int status, i;

    if (! pid) pid = &p;

    // Generate the array of argument strings before we do any executing
    arguments[0] = script;
    for (i = 0; i < numArgs; i++) arguments[i + 1] = fileArgs[i];
    arguments[i + 1] = NULL;

    *pid = fork(); //open fork

    if (*pid == (pid_t)-1) exit(13); //error
    else if (*pid == 0) { //child process started
        execve(arguments[0], arguments, environ);
        exit(13); //if we reach this point, there's an error
    }

    wpid = waitpid(*pid, &status, 0); //wait while child process finishes

    if (wpid == (pid_t)-1) return wpid;
    return (OSErr)WEXITSTATUS(status);
}

#pragma mark -

///////////////////////////////////////
// This function loads all the neccesary settings
// from config files in the Resources folder
///////////////////////////////////////
static void GetParameters (void)
{
    char *str;
    if (! (str = (char *)GetScript())) //get path to script to be executed
        RedFatalAlert("\pInitialization Error",
                      "\pError getting script from application bundle.");
    strcpy((char *)&scriptPath, str);

    if (! (str = (char *)GetOpenDoc())) //get path to openDoc
        RedFatalAlert("\pInitialization Error",
                      "\pError getting openDoc from application bundle.");
    strcpy((char *)&openDocPath, str);
}

///////////////////////////////////////
// Get path to the script in Resources folder
///////////////////////////////////////
static unsigned char* GetScript (void)
{
    CFStringRef fileName;
    CFBundleRef appBundle;
    CFURLRef scriptFileURL;
    FSRef fileRef;
    unsigned char *path;

    //get CF URL for script
    if (! (appBundle = CFBundleGetMainBundle())) return NULL;
    if (! (fileName = CFStringCreateWithCString(NULL, kScriptFileName,
                                                kCFStringEncodingASCII)))
        return NULL;
    if (! (scriptFileURL = CFBundleCopyResourceURL(appBundle, fileName, NULL,
                                                   NULL))) return NULL;

    //Get file reference from Core Foundation URL
    if (! CFURLGetFSRef(scriptFileURL, &fileRef)) return NULL;

    //dispose of the CF variables
    CFRelease(scriptFileURL);
    CFRelease(fileName);

    //create path string
    if (! (path = malloc(kMaxPathLength))) return NULL;
    if (FSMakePath(fileRef, path, kMaxPathLength)) return NULL;
    if (! DoesFileExist(path)) return NULL;

    return path;
}

///////////////////////////////////////
// Gets the path to openDoc in Resources folder
///////////////////////////////////////
static unsigned char* GetOpenDoc (void)
{
    CFStringRef fileName;
    CFBundleRef appBundle;
    CFURLRef openDocFileURL;
    FSRef fileRef;
    unsigned char *path;

    //get CF URL for openDoc
    if (! (appBundle = CFBundleGetMainBundle())) return NULL;
    if (! (fileName = CFStringCreateWithCString(NULL, kOpenDocFileName,
                                                kCFStringEncodingASCII)))
        return NULL;
    if (! (openDocFileURL = CFBundleCopyResourceURL(appBundle, fileName, NULL,
                                                    NULL))) return NULL;

    //Get file reference from Core Foundation URL
    if (! CFURLGetFSRef( openDocFileURL, &fileRef )) return NULL;

    //dispose of the CF variables
    CFRelease(openDocFileURL);
    CFRelease(fileName);

    //create path string
    if (! (path = malloc(kMaxPathLength))) return NULL;
    if (FSMakePath(fileRef, path, kMaxPathLength)) return NULL;
    if (! DoesFileExist(path)) return NULL;

    return path;
}

#if MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_6
///////////////////////////////////////
// Gets the path to XQuartzFixer in Resources folder
///////////////////////////////////////
static CFStringRef GetXQuartzFixer (void)
{
    CFBundleRef appBundle;
    CFURLRef XQuartzFixerFileURL;
    CFStringRef path;

    //get CF URL for XQuartzFixer
    if (! (appBundle = CFBundleGetMainBundle())) return NULL;
    if (! (XQuartzFixerFileURL = CFBundleCopyResourceURL(appBundle, kXQuartzFixerFileName, NULL,
                                                    NULL))) return NULL;

    //Get file system path from Core Foundation URL
    path = CFURLCopyFileSystemPath( XQuartzFixerFileURL, kCFURLPOSIXPathStyle );

    //dispose of the CF variables
    CFRelease(XQuartzFixerFileURL);

    return path;
}
#endif

#pragma mark -

/////////////////////////////////////
// Load menu bar from nib
/////////////////////////////////////
OSErr LoadMenuBar (char *appName)
{
    OSErr err;
    IBNibRef nibRef;

    if (err = CreateNibReference(CFSTR("MenuBar"), &nibRef)) return err;
    if (err = SetMenuBarFromNib(nibRef, CFSTR("MenuBar"))) return err;
    DisposeNibReference(nibRef);

    return noErr;
}

#pragma mark -

///////////////////////////////////////
// Generate path string from FSSpec record
///////////////////////////////////////
static OSStatus FSMakePath(FSRef fileRef, unsigned char *path, long maxPathSize)
{
    // and then convert the FSRef to a path
    return FSRefMakePath(&fileRef, path, maxPathSize);
}

////////////////////////////////////////
// Standard red error alert, then exit application
////////////////////////////////////////
static void RedFatalAlert (Str255 errorString, Str255 expStr)
{
    StandardAlert(kAlertStopAlert, errorString,  expStr, NULL, NULL);
    ExitToShell();
}

///////////////////////////////////////
// Determines whether file exists at path or not
///////////////////////////////////////
static short DoesFileExist (unsigned char *path)
{
    if (access((char *)path, F_OK) == -1) return false;
    return true;
}

#pragma mark -

///////////////////////////////////////
// Apple Event handler for Quit i.e. from
// the dock or Application menu item
///////////////////////////////////////
static OSErr AppQuitAEHandler(const AppleEvent *theAppleEvent,
                              AppleEvent *reply, long refCon)
{
    #pragma unused (reply, refCon, theAppleEvent)

    while (numArgs > 0) free(fileArgs[numArgs--]);

    if (! taskDone && pid) { //kill the script process brutally
        kill(pid, 9);
        printf("Wireshark.app: PID %d killed brutally\n", pid);
    }

    pthread_cancel(tid);
    if (odtid) pthread_cancel(odtid);

    ExitToShell();

    return noErr;
}

/////////////////////////////////////
// Handler for docs dragged on app icon
/////////////////////////////////////
static OSErr AppOpenDocAEHandler(const AppleEvent *theAppleEvent,
                                 AppleEvent *reply, long refCon)
{
    #pragma unused (reply, refCon)

    OSErr err = noErr;
    AEDescList fileRefList;
    AEKeyword keyword;
    DescType type;

    short i;
    long count, actualSize;

    FSRef fileRef;
    unsigned char path[kMaxPathLength];

    while (numArgs > 0) free(fileArgs[numArgs--]);

    //Read the AppleEvent
    err = AEGetParamDesc(theAppleEvent, keyDirectObject, typeAEList,
                         &fileRefList);

    err = AECountItems(&fileRefList, &count); //Count number of files

    for (i = 1; i <= count; i++) { //iteratively process each file
        //get fsref from apple event
        if (! (err = AEGetNthPtr(&fileRefList, i, typeFSRef, &keyword, &type,
                                 (Ptr)&fileRef, sizeof(FSRef), &actualSize)))
        {
            //get path from file ref
            if ((err = FSMakePath(fileRef, (unsigned char *)&path,
                                  kMaxPathLength))) return err;

            if (numArgs == kMaxArgumentsToScript) break;

            if (! (fileArgs[numArgs] = malloc(kMaxPathLength))) return true;

            strcpy(fileArgs[numArgs++], (char *)&path);
        }
        else return err;
    }

    if (! taskDone) pthread_create(&odtid, NULL, OpenDoc, NULL);
    else pthread_create(&tid, NULL, Execute, NULL);

    return err;
}

///////////////////////////////
// Handler for clicking on app icon
///////////////////////////////
// if app is already open
static OSErr AppReopenAppAEHandler(const AppleEvent *theAppleEvent,
                                 AppleEvent *reply, long refCon)
{
    return runScript();
}

// if app is being opened
static OSErr AppOpenAppAEHandler(const AppleEvent *theAppleEvent,
                                 AppleEvent *reply, long refCon)
{
    #pragma unused (reply, refCon, theAppleEvent)

    // the app has been opened without any items dragged on to it
    pthread_create(&tid, NULL, Execute, NULL);

    return noErr;
}


static void OpenURL(Str255 url)
{
	// Use Internet Config to hand the URL to the appropriate application, as
	// set by the user in the Internet Preferences pane.
	ICInstance icInstance;
	// Applications creator code:
	OSType signature = 'Inks';
	OSStatus error = ICStart( &icInstance, signature );
	if ( error == noErr )
	{
		ConstStr255Param hint = 0x0;
		const char* data = url;
		long length = strlen(url);
		long start =  0;
		long end = length;
		// Don't bother testing return value (error); launched application will
		// report problems.
		ICLaunchURL( icInstance, hint, data, length, &start, &end );
		ICStop( icInstance );
	}
}


//////////////////////////////////
// Handler for when X11 fails to start
// Applies only to pre-Leopard OS X, as it's auto-started
// through launchd in Leopard and later
//////////////////////////////////
static OSStatus X11FailedHandler(EventHandlerCallRef theHandlerCall,
                                 EventRef theEvent, void *userData)
{
    #pragma unused(theHanderCall, theEvent, userData)

    pthread_join(tid, NULL);
    if (odtid) pthread_join(odtid, NULL);

	SInt16 itemHit;
	const unsigned char *getX11 = "\pGet X11 for Panther";

	AlertStdAlertParamRec params;
	params.movable = true;
	params.helpButton = false;
	params.filterProc = NULL;
	params.defaultText = (StringPtr) kAlertDefaultOKText;
	params.cancelText = getX11;
	params.otherText = NULL;
	params.defaultButton = kAlertStdAlertOKButton;
	params.cancelButton = kAlertStdAlertCancelButton;
	params.position = kWindowDefaultPosition;

	StandardAlert(kAlertStopAlert, "\pFailed to start X11",
			"\pWireshark.app requires Apple's X11, which is freely downloadable from Apple's website for Panther (10.3.x) users and available as an optional install from the installation DVD for Tiger (10.4.x) users.\n\nPlease install X11 and restart Wireshark.",
			&params, &itemHit);

	if (itemHit == kAlertStdAlertCancelButton)
	{
		OpenURL("http://www.apple.com/downloads/macosx/apple/macosx_updates/x11formacosx.html");
	}

    ExitToShell();


    return noErr;
}


// Compile and run a small AppleScript. The code below does no cleanup and no proper error checks
// but since it's there until the app is shut down, and since we know the script is okay,
// there should not be any problems.
ComponentInstance theComponent;
AEDesc scriptTextDesc;
OSStatus err;
OSAID scriptID, resultID;

static OSStatus CompileAppleScript(const void* text, long textLength,
                                  AEDesc *resultData) {

    resultData = NULL;
    /* set up locals to a known state */
    theComponent = NULL;
    AECreateDesc(typeNull, NULL, 0, &scriptTextDesc);
    scriptID = kOSANullScript;
    resultID = kOSANullScript;

    /* open the scripting component */
    theComponent = OpenDefaultComponent(kOSAComponentType,
                                        typeAppleScript);
    if (theComponent == NULL) { err = paramErr; return err; }

    /* put the script text into an aedesc */
    err = AECreateDesc(typeChar, text, textLength, &scriptTextDesc);
    if (err != noErr) return err;

    /* compile the script */
    err = OSACompile(theComponent, &scriptTextDesc,
                     kOSAModeNull, &scriptID);

    return err;
}

/* runs the compiled applescript */
static OSErr runScript()
{
    /* run the script */
    err = OSAExecute(theComponent, scriptID, kOSANullScript,
                     kOSAModeNull, &resultID);
    return err;
}


/* Simple shortcut to the function that actually compiles the applescript. */
static OSStatus SimpleCompileAppleScript(const char* theScript) {
    return CompileAppleScript(theScript, strlen(theScript), NULL);
}
