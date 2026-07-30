// switchmio - Opens web.strem.io using the Switch's built-in Web Applet
#include <switch.h>
#include <stdio.h>
#include <string.h>

#define WEBSITE_URL "https://web.strem.io/"

int main(int argc, char* argv[])
{
    consoleInit(NULL);

    printf("switchmio\n");
    printf("Opening: %s\n\n", WEBSITE_URL);
    consoleUpdate(NULL);

    WebCommonConfig config;
    Result rc = webPageCreate(&config, WEBSITE_URL);

    if (R_SUCCEEDED(rc)) {
        webConfigSetWhitelist(&config, "^http*");
        webConfigSetFooter(&config, true);

        WebCommonReply reply;
        rc = webConfigShow(&config, &reply);

        if (R_FAILED(rc)) {
            printf("Failed to show web applet: 0x%x\n", rc);
            consoleUpdate(NULL);
            svcSleepThread(3e9);
        }
    } else {
        printf("Failed to create web config: 0x%x\n", rc);
        consoleUpdate(NULL);
        svcSleepThread(3e9);
    }

    consoleExit(NULL);
    return 0;
}
