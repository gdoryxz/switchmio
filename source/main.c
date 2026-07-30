// switchmio - Opens Stremio's web app using the Switch's built-in Web Applet
// Change WEBSITE_URL below if you ever want to point it elsewhere.

#include <switch.h>
#include <stdio.h>
#include <string.h>

// ==== EDIT THIS ====
#define WEBSITE_URL "https://web.stremio.com/"
// ===================

int main(int argc, char* argv[])
{
    // Init console output (in case the applet fails, so we can show an error)
    consoleInit(NULL);

    printf("switchmio\n");
    printf("Opening: %s\n\n", WEBSITE_URL);
    consoleUpdate(NULL);

    // Configure and launch the system Web Applet
    WebCommonConfig config;
    Result rc = webPageCreate(&config, WEBSITE_URL);

    if (R_SUCCEEDED(rc)) {
        // Optional tweaks:
        webConfigSetWhitelist(&config, "^http*");   // allow navigating to any http(s) link
        webConfigSetFooter(&config, true);          // show footer controls
        // webConfigSetJsExtension(&config, true);   // uncomment if you need extra JS APIs

        WebCommonReply reply;
        rc = webConfigShow(&config, &reply);

        if (R_FAILED(rc)) {
            printf("Failed to show web applet: 0x%x\n", rc);
            consoleUpdate(NULL);
            svcSleepThread(3e9); // pause 3 sec so user can read the error
        }
    } else {
        printf("Failed to create web config: 0x%x\n", rc);
        consoleUpdate(NULL);
        svcSleepThread(3e9);
    }

    consoleExit(NULL);
    return 0;
}
