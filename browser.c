/**
 *  VDR Skin Plugin which uses CEF OSR as rendering engine
 *
 *  browser.cpp
 *
 *  (c) 2019 Robert Hannebauer
 *
 * This code is distributed under the terms and conditions of the
 * GNU GENERAL PUBLIC LICENSE. See the file COPYING for details.
 *
 **/

#include <chrono>
#include <thread>
#include <vdr/plugin.h>
#include <nanomsg/nn.h>
#include <nanomsg/reqrep.h>
#include <nanomsg/pipeline.h>
#include <unistd.h>
#include "browser.h"
#include "hbbtvservice.h"

bool DumpDebugData = true;

int Browser::osdWidth;
int Browser::osdHeight;

Browser *upd;

Browser::Browser() {
    // update thread
    nngsocket = new NngSocket();
    upd = this;
    showBrowser();
}

Browser::~Browser() {
    DELETENULL(nngsocket);

    hideBrowser();
    stopUpdate();

    upd = nullptr;

    if (osd != nullptr) {
        delete osd;
        osd = nullptr;
    }
}

bool Browser::loadPage(std::string url, int rootFontSize) {
    std::string cmdUrl("URL ");
    cmdUrl.append(url);

    sendCommand(cmdUrl.c_str());

    if (rootFontSize > 0) {
        setRootFontSize(rootFontSize);
    }

    return true;
}

bool Browser::hideBrowser() {
    return sendCommand("PAUSE");
}

bool Browser::showBrowser() {
    return sendCommand("RESUME");
}

void Browser::callRawJavascript(cString script) {
    char *cmd;
    asprintf(&cmd, "JS %s", *script);
    sendCommand(cmd);
    free(cmd);
}

bool Browser::setBrowserSize(int width, int height) {
    char *cmd;

    osdWidth = width;
    osdHeight = height;

    hideBrowser();

    asprintf(&cmd, "SIZE %d %d", osdWidth, osdHeight);
    auto result = sendCommand(cmd);
    free(cmd);

    showBrowser();

    return result;
}

bool Browser::setZoomLevel(double zoom) {
    char *cmd;

    asprintf(&cmd, "ZOOM %f", zoom);
    auto result = sendCommand(cmd);
    free(cmd);

    return result;
}

bool Browser::setRootFontSize(int px) {
    char *cmd;

    asprintf(&cmd, "document.getElementsByTagName('html').item(0).style.fontSize = '%dpx'", px);
    callRawJavascript(cmd);
    free(cmd);

    return true;
}

bool Browser::sendKeyEvent(cString key) {
    char *cmd;

    asprintf(&cmd, "KEY %s", *key);
    auto result = sendCommand(cmd);
    free(cmd);

    return result;
}

bool Browser::sendCommand(const char* command) {
    bool returnValue;

    dbgbrowser("Send command '%s'\n", command);

    char *response = nullptr;
    int bytes;

    if ((bytes = nn_send(NngSocket::getCommandSocket(), command, strlen(command) + 1, 0)) < 0) {
        esyslog("Unable to send command...");
        return false;
    }

    if (bytes > 0 && (bytes = nn_recv(NngSocket::getCommandSocket(), &response, NN_MSG, 0)) < 0) {
        esyslog("Unable to read response...");
        returnValue = false;
    } else {
        dbgbrowser("Response received: '%s', %d\n", response, bytes);

        returnValue = strcasecmp(response, "ok") == 0;
    }

    if (response) {
        nn_freemsg(response);
    }

    return returnValue;
}

void Browser::startUpdate(int left, int top, int width, int height) {
    setBrowserSize(width, height);

    // try to calculate an appropriate zoom level
    // Full HD is 1920 x 1080 = 2073600 Pixel
    auto newPixel = (double)width * (double)height;
    auto zoom = sqrt(newPixel / 2073600.0);
    setZoomLevel(zoom);

    osd = cOsdProvider::NewOsd(left, top);

    tArea Area = { 0, 0, width - 1, height - 1, 32 };
    osd->SetAreas(&Area, 1);

    cRect rect(0, 0, width, height);
    pixmap = osd->CreatePixmap(0, rect, rect);

    isRunning = true;
    updateThread = new std::thread(readStream, osdWidth, pixmap);
    statusThread = new std::thread(readBrowserMessage);
}

void Browser::stopUpdate() {
    if (!isRunning) {
        return;
    }

    isRunning = false;
    updateThread->join();
}

void Browser::FlushOsd() {
    osd->Flush();
}

void Browser::readStream(int width, cPixmap *destPixmap) {
    int bytes;

    // read count of dirty rects
    while(upd->isRunning) {
        unsigned long dirtyRecs = 0;
        if ((bytes = nn_recv(NngSocket::getStreamSocket(), &dirtyRecs, sizeof(dirtyRecs), 0)) > 0) {
            // sanity check: If dirtyRecs > 20 then ignore this
            if (dirtyRecs > 20) {
                // FIXME: Try to clear the input buffer to get a new valid state
                continue;
            }

            for (unsigned long i = 0; i < dirtyRecs; ++i) {
                // read coordinates and size
                int x, y, w, h;
                if ((bytes = nn_recv(NngSocket::getStreamSocket(), &x, sizeof(x), 0)) > 0) {
                }

                if ((bytes = nn_recv(NngSocket::getStreamSocket(), &y, sizeof(y), 0)) > 0) {
                }

                if ((bytes = nn_recv(NngSocket::getStreamSocket(), &w, sizeof(w), 0)) > 0) {
                }

                if ((bytes = nn_recv(NngSocket::getStreamSocket(), &h, sizeof(h), 0)) > 0) {
                }

                // dsyslog("Received dirty rec: (x %d, y %d) -> (w %d, h %d)\n", x, y, w, h);
                // dbgbrowser("Received dirty rec: (x %d, y %d) -> (w %d, h %d)\n", x, y, w, h);

                // create image from input data
                cSize recImageSize(w, h);
                cPoint recPoint(x, y);
                const cImage recImage(recImageSize);
                auto *data2 = const_cast<tColor*>(recImage.Data());

                for (int j = 0; j < h; ++j) {
                    if ((bytes = nn_recv(NngSocket::getStreamSocket(), data2 + (w * j), 4 * w, 0)) > 0) {
                        // everything is fine
                    } else {
                        // TODO: Und nun?
                    }
                }

                destPixmap->DrawImage(recPoint, recImage);

                upd->osd->Flush();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

void Browser::readBrowserMessage() {
    int bytes;

    // read status message from browser
    while(upd->isRunning) {
        char *buf = NULL;
        if ((bytes = nn_recv(NngSocket::getStatusSocket(), &buf, NN_MSG, 0)) > 0) {
            BrowserStatus_v1_0 status;
            status.message = cString(buf);

            if (strncmp(status.message, "PLAY_VIDEO:", 11) == 0) {
                // a video shall be displayed.
                upd->stopUpdate();
            }

            cPluginManager::CallAllServices("BrowserStatus-1.0", &status);

            nn_freemsg (buf);
        }
    }
}