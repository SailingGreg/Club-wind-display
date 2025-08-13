
#include <WiFi.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <TFT_eSPI.h> // Seeed Arduino LCD library
//#include <M5GFX.h>
#include "wifi_credentials.h" 

#define USE_TFT_ESPI_LIBRARY

#define USE_SERIAL Serial

//const char *ssid = "AbleTasman2";
//const char *password = "Anyth3ng";

HTTPClient http;
PNG png;

#ifdef EPAPER_ENABLE // Only compile this code if the EPAPER_ENABLE is defined in User_Setup.h
EPaper epaper;
#endif

//M5GFX display;
//TFT_eSPI tft = TFT_eSPI();

// Add offset variables at the top
int imageOffsetX = 0;
int imageOffsetY = 40;  // Leave 40 pixels for header

// Minimal memory approach - just one line at a time
#define MAX_LINE_WIDTH 800
uint8_t* lineBuffer = nullptr; // unin16_t as pushImage() require this
//uint8_t* lineBuffer = nullptr;
bool displayInitialized = false;

const char* urls[] = {
    {"http://devserver"},
    {"http://ccycone"}
};
const char* serverBase;

const char* windSpeed = "/wind/daywind.png";
const char* windDir = "/wind/daywinddir.png";

// Function to build full URLs
String buildURL(const char* path) {
    return String(serverBase) + String(path);
}

bool connectWiFi() {
    struct WiFiNetwork {
        const char* ssid;
        const char* password;
    };
    
    WiFiNetwork networks[] = {
        {ssid1, password1},
        {ssid2, password2}

    };

    const int networkCount = sizeof(networks) / sizeof(networks[0]);
    const int urlCount = sizeof(urls) / sizeof(urls[0]);

    // Add safety check
    if (networkCount != urlCount) {
        Serial.println("[ERROR] WiFi networks and URL count mismatch!");
    }
    
    for (int net = 0; net < networkCount; net++) {
        Serial.printf("[WiFi] Trying network: %s\n", networks[net].ssid);
        
        WiFi.begin(networks[net].ssid, networks[net].password);
        
        int attempts = 10;
        while (attempts > 0 && WiFi.status() != WL_CONNECTED) {
            delay(500);
            attempts--;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WiFi] Connected to: %s\n", networks[net].ssid);
            Serial.printf("[WiFi] IP address: %s\n", WiFi.localIP().toString().c_str());
            serverBase = urls[net]; // flag the appropriate server
            Serial.printf("[SERVER] Using server: %s\n", serverBase);
            return true;
        }
        
        Serial.printf("[WiFi] Failed to connect to: %s\n", networks[net].ssid);
    }
    
    Serial.println("[WiFi] All networks failed!");
    return false;
}

bool ensureWiFiConnected() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Connection lost, attempting to reconnect...");
        
        WiFi.disconnect();
        delay(1000);
        
        return connectWiFi();
    }
    return true;
}

bool downloadImage(const char* url, uint8_t** imageData, int* imageSize) {
    USE_SERIAL.printf("[HTTP] Downloading: %s\n", url);
    
    Serial.printf("[MEM] Free heap before download: %d bytes\n", ESP.getFreeHeap());
    
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        USE_SERIAL.printf("[HTTP] GET failed, error: %s\n", 
                         http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    int len = http.getSize();
    if (len <= 0) {
        Serial.println("[HTTP] No content or unknown length");
        http.end();
        return false;
    }

    Serial.printf("[HTTP] Content length: %d bytes\n", len);

    // Check if we have enough memory
    if (ESP.getFreeHeap() < (len + 10000)) {
        Serial.printf("[HTTP] Insufficient memory for download\n");
        http.end();
        return false;
    }

    *imageData = (uint8_t*)malloc(len);
    if (*imageData == nullptr) {
        Serial.println("[HTTP] Failed to allocate memory for download");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buff[512];
    int totalRead = 0;

    while (totalRead < len && stream->connected()) {
        size_t available = stream->available();
        
        if (available) {
            int readSize = min(available, sizeof(buff));
            readSize = min(readSize, len - totalRead);
            
            int bytesRead = stream->readBytes(buff, readSize);
            if (bytesRead > 0) {
                memcpy(*imageData + totalRead, buff, bytesRead);
                totalRead += bytesRead;
            }
        } else {
            delay(10);
        }
        
        // Yield to prevent watchdog
        if (totalRead % 5000 == 0) {
            yield();
        }
    }

    http.end();
    
    if (totalRead == len) {
        Serial.printf("[HTTP] Download complete: %d bytes\n", totalRead);
        *imageSize = len;
        return true;
    } else {
        Serial.printf("[HTTP] Download failed: %d/%d bytes\n", totalRead, len);
        free(*imageData);
        *imageData = nullptr;
        return false;
    }
}

// Updated PNG callback with offset support
int pngDrawStream(PNGDRAW *pDraw) {
    uint8_t *pPixels = (uint8_t *)pDraw->pPixels;
    int lineWidth = min(pDraw->iWidth, MAX_LINE_WIDTH);
    
    float scale = 1.1f;
    
    // Process RGB data and draw scaled pixels with offset
    for (int x = 0; x < lineWidth; x++) {
        uint8_t pixelValue;
        
        int pixelIndex = x * 3;
        if (pixelIndex + 2 < lineWidth * 3) {
            uint8_t r = pPixels[pixelIndex];
            pixelValue = r;
        } else {
            pixelValue = pPixels[x];
        }
        
        uint16_t color = (pixelValue < 0xF0) ? 0x0000 : 0xFFFF;
        
        // Calculate scaled positions WITH OFFSET
        int scaledX1 = imageOffsetX + (int)(x * scale);
        int scaledX2 = imageOffsetX + (int)((x + 1) * scale);
        int scaledY1 = imageOffsetY + (int)(pDraw->y * scale);
        int scaledY2 = imageOffsetY + (int)((pDraw->y + 1) * scale);
        
        // Draw scaled pixel with offset
        for (int sy = scaledY1; sy < scaledY2; sy++) {
            for (int sx = scaledX1; sx < scaledX2; sx++) {
                if (sx < 800 && sy < 480) {
                    epaper.drawPixel(sx, sy, color);
                }
            }
        }
    }
    
    if (pDraw->y % 25 == 0) {
        Serial.printf("[PNG] Line: %d (%.1fx scaled, offset %d,%d)\n", 
                      pDraw->y, scale, imageOffsetX, imageOffsetY);
        yield();
    }
    
    return 1;
}

// Function to draw header text
void drawHeader() {
    Serial.println("[HEADER] Drawing header text...");
    
    // Set text properties
    epaper.setTextColor(TFT_BLACK, TFT_WHITE);
    //epaper.setFreeFont(&FreeSans9pt7b);
    //epaper.setFreeFont(&FreeSans9pt7b);    // Smaller than 9pt
    //epaper.setTextSize(3);  // Adjust size as needed

    epaper.setTextFont(4); 
    epaper.setTextSize(1);
    
    // Get current time or use static text
    String headerText = "Club Wind";
    String timeText = "Updated: " + String(millis() / 60000) + " min";  // Simple uptime
    
    // Draw header text
    //epaper.setCursor(10, 10);
    //epaper.print(headerText);

    epaper.setCursor(10, 5);
    epaper.print(headerText);
    epaper.setCursor(11, 5);  // Draw again 1 pixel right
    epaper.print(headerText);
    
    // Draw timestamp on right side
    //epaper.setTextSize(1);
    //epaper.setCursor(500, 10);  // Adjust position as needed
    //epaper.print(timeText);

    // Reset to default font for other text if needed
    epaper.setFreeFont();
    
    // Optional: Draw a line under the header
    //epaper.drawLine(0, 35, 800, 35, TFT_BLACK);
}


// pixel and scaled - not simple option
/*int pngDrawStream(PNGDRAW *pDraw) {
    uint8_t *pPixels = (uint8_t *)pDraw->pPixels;
    int lineWidth = min(pDraw->iWidth, MAX_LINE_WIDTH);
    
    // 1.5x scaling: 350×180 becomes 525×270
    float scale = 1.2f;
    
    // Process RGB data and draw scaled pixels
    for (int x = 0; x < lineWidth; x++) {
        uint8_t pixelValue;
        
        int pixelIndex = x * 3;
        if (pixelIndex + 2 < lineWidth * 3) {
            uint8_t r = pPixels[pixelIndex];
            pixelValue = r;  // Just use R channel for grayscale
        } else {
            pixelValue = pPixels[x];
        }
        
        // Convert to black/white
        uint16_t color = (pixelValue < 0xF0) ? 0x0000 : 0xFFFF;
        
        // Calculate scaled positions
        int scaledX1 = (int)(x * scale);
        int scaledX2 = (int)((x + 1) * scale);
        int scaledY1 = (int)(pDraw->y * scale);
        int scaledY2 = (int)((pDraw->y + 1) * scale);
        
        // Draw this pixel scaled (fill the scaled rectangle)
        for (int sy = scaledY1; sy < scaledY2; sy++) {
            for (int sx = scaledX1; sx < scaledX2; sx++) {
                // Make sure we don't go outside display bounds
                if (sx < 800 && sy < 480) {  // Seeed display is 800×480
                    epaper.drawPixel(sx, sy, color);
                }
            }
        }
    }
    
    if (pDraw->y % 25 == 0) {
        Serial.printf("[PNG] Line: %d (%f x scaled)\n", pDraw->y, scale);
        yield();
    }
    
    return 1;
}*/

// test if 3 btyes - this works to an extent
/*int pngDrawStream(PNGDRAW *pDraw) {
    uint8_t *pPixels = (uint8_t *)pDraw->pPixels;
    int lineWidth = min(pDraw->iWidth, MAX_LINE_WIDTH);

    // Cast lineBuffer to uint16_t for proper processing
    //uint16_t* line16 = (uint16_t*)lineBuffer;
    
    if (pDraw->y == 100) {
        Serial.printf("[PNG] Testing multi-byte formats:\n");
        Serial.printf("[PNG] Direct bytes: ");
        for (int i = 0; i < 15; i++) {
            Serial.printf("%02X ", pPixels[i]);
        }
        Serial.println();
        
        Serial.printf("[PNG] As RGB (every 3rd): ");
        for (int i = 0; i < 15; i += 3) {
            Serial.printf("(%02X,%02X,%02X) ", pPixels[i], pPixels[i+1], pPixels[i+2]);
        }
        Serial.println();
    }
    
    // Try processing as RGB data (3 bytes per pixel)
    for (int x = 0; x < lineWidth; x++) {
        uint8_t pixelValue;
        
        // Assume RGB format: take the R channel as grayscale
        int pixelIndex = x * 3;
        if (pixelIndex + 2 < lineWidth * 3) {
            uint8_t r = pPixels[pixelIndex];
            uint8_t g = pPixels[pixelIndex + 1]; 
            uint8_t b = pPixels[pixelIndex + 2];
            
            // Convert RGB to grayscale
            pixelValue = (r * 77 + g * 151 + b * 28) >> 8;
        } else {
            pixelValue = pPixels[x];  // Fallback
        }
        
        // Process the grayscale value
        if (pixelValue < 0xF0) {
            lineBuffer[x] = 0x00;  // Black
        } else {
            lineBuffer[x] = 0xFF;  // White
        }

        // Draw pixel directly (we know this works)
        uint16_t color = (pixelValue < 0xF0) ? 0x0000 : 0xFFFF;
        epaper.drawPixel(x, pDraw->y, color);
    }
    
    //epaper.pushImage(0, pDraw->y, lineWidth, 1, lineBuffer, false, NULL);
    //epaper.pushImage(0, pDraw->y, lineWidth, 1, (uint16_t *) lineBuffer);

    if (pDraw->y % 25 == 0) {
        Serial.printf("[PNG] Line: %d\n", pDraw->y);
        yield();
    }
    
    return 1;
}*/

bool displayPNGStreaming(uint8_t* data, int dataSize) {
    Serial.printf("[MEM] Free heap before PNG decode: %d bytes\n", ESP.getFreeHeap());
    
    //lineBuffer = (uint8_t*)malloc(MAX_LINE_WIDTH * sizeof(uint8_t));
    // Allocate as uint16_t for Seeed e-paper
    lineBuffer = (uint8_t*)malloc(MAX_LINE_WIDTH * sizeof(uint16_t));  // 2 bytes per pixel
    if (lineBuffer == nullptr) {
        Serial.println("[PNG] Failed to allocate line buffer");
        return false;
    }
    
    displayInitialized = false;
    
    int rc = png.openRAM(data, dataSize, pngDrawStream);
    if (rc != PNG_SUCCESS) {
        Serial.printf("[PNG] Failed to open PNG: %d\n", rc);
        free(lineBuffer);
        return false;
    }
    
    // DEBUG: Check the actual PNG dimensions
    int pngWidth = png.getWidth();
    int pngHeight = png.getHeight();
    Serial.printf("[PNG] PNG opened: %dx%d, %d bpp, alpha: %d\n", 
                  pngWidth, pngHeight, png.getBpp(), png.hasAlpha());
    
    // DEBUG: Check what lineWidth will be used
    int expectedLineWidth = min(pngWidth, MAX_LINE_WIDTH);
    Serial.printf("[PNG] Expected line width: %d (PNG width: %d, MAX_LINE_WIDTH: %d)\n", 
                  expectedLineWidth, pngWidth, MAX_LINE_WIDTH);
    
    epaper.startWrite();
    Serial.println("[PNG] Starting streaming decode...");
    rc = png.decode(NULL, PNG_PIXEL_GRAYSCALE);
    epaper.endWrite();
    png.close();
    
    // ... rest of function
        if (rc != PNG_SUCCESS) {
        Serial.printf("[PNG] Decode failed: %d\n", rc);
        free(lineBuffer);
        return false;
    }
    
    // Refresh the e-paper display
    Serial.println("[PNG] Refreshing e-paper display...");
    epaper.update(); // M5GFX method for updating e-paper
    
    // Clean up
    free(lineBuffer);
    lineBuffer = nullptr;
    
    Serial.println("[PNG] Streaming display complete!");
    return true;
}


void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("=== M5Paper3 PNG Display ===");
    
    //Serial.printf("[MEM] Total heap: %d bytes\n", ESP.getHeapSize());
    //Serial.printf("[MEM] Free heap at startup: %d bytes\n", ESP.getFreeHeap());

    Serial.printf("[MEM] Total heap: %d bytes\n", ESP.getHeapSize());
    Serial.printf("[MEM] Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("[MEM] PSRAM size: %d bytes\n", ESP.getPsramSize());
    Serial.printf("[MEM] Free PSRAM: %d bytes\n", ESP.getFreePsram());

    Serial.println("[FONTS] Font support check:");
    
    #ifdef SMOOTH_FONT
        Serial.println("SMOOTH_FONT is enabled");
    #else
        Serial.println("SMOOTH_FONT is NOT enabled");
    #endif
    
    #ifdef LOAD_GFXFF
        Serial.println("GFXFF (FreeFonts) enabled");
    #else
        Serial.println("GFXFF (FreeFonts) NOT enabled");
    #endif
    // Test if Free_Fonts.h is available
    #ifdef _FREE_FONTS_H_
        Serial.println("Free_Fonts.h is included ✓");
    #else
        Serial.println("Free_Fonts.h is NOT included ✗");
    #endif

    // Initialize M5Paper3
    epaper.begin();

    epaper.setTextColor(TFT_BLACK, TFT_WHITE); // Adding a background colour erases previous text automatically
    
    /*
    if (display.isEPD()) {
        display.setEpdMode(epd_mode_t::epd_fastest);
    }
    if (display.width() < display.height()) { // change to portraist from landscape
        display.setRotation(display.getRotation() ^ 1);
    }*/
    
    epaper.fillScreen(TFT_WHITE);
    epaper.update(); // Update the display

    // deal with endness?
    //epaper.setSwapBytes(false);

    // Connect to WiFi
    if (!connectWiFi()) {
        Serial.println("WiFi failed - showing blank screen");
        epaper.fillScreen(TFT_WHITE);
        epaper.update();
        delay(3000);
        return;
    }

    /*if (connectWiFi()) {
        testDisplayWidth();  // Test first
        delay (3000);
    }*/

    // Download and display PNG
    uint8_t* imageData = nullptr;
    int imageSize = 0;

    // Draw header BEFORE downloading image
    drawHeader();
    

    Serial.printf("[MEM] Final free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.println("=== Setup Complete ===");
}

/*void loop() {
    // Minimal loop - just check memory occasionally
    delay(60000); // 1 minute intervals
    Serial.printf("[LOOP] Free heap: %d bytes\n", ESP.getFreeHeap());
    
    // For production, add periodic refresh logic here
    // Example: refresh weather data every 15 minutes
    uint8_t* imageData = nullptr;
    int imageSize = 0;

    // ensure old image removed
    epaper.fillScreen(TFT_WHITE);
    epaper.update(); // Update the display
    
    if (downloadImage("http://devserver/wind/daywind.png", &imageData, &imageSize)) {
        Serial.println("[MAIN] Download successful");
        
        //if (displayPNGFullImage(imageData, imageSize)) {
        if (displayPNGStreaming(imageData, imageSize)) {
            Serial.println("[MAIN] Image streamed successfully");
        } else {
            Serial.println("[MAIN] Image streaming failed");
            epaper.fillScreen(TFT_WHITE);
            epaper.update();
            delay(300000);
        }
        
        free(imageData);  // alloced in the download function
    } else {
        Serial.println("[MAIN] Download failed");
        epaper.fillScreen(TFT_WHITE);
        epaper.update();
        delay(300000);
    }

    Serial.printf("[MEM] Final free heap: %d bytes\n", ESP.getFreeHeap());
    //Serial.println("=== Setup Complete ===");
}*/

// Updated loop with header support
/*void loop() {
    static int refreshCount = 0;
    
    delay(300000); // 5 minute intervals
    Serial.printf("[LOOP] Free heap: %d bytes\n", ESP.getFreeHeap());
    
    refreshCount++;
    
    // Force a full refresh every 12 updates (1 hour) to clear ghosting
    bool forceFullRefresh = (refreshCount % 12 == 0);
    
    if (forceFullRefresh) {
        Serial.println("[REFRESH] Performing full refresh to clear ghosting");
        
        // Full refresh cycle: white -> black -> white
        epaper.fillScreen(TFT_BLACK);
        epaper.update();
        delay(2000);
        
        epaper.fillScreen(TFT_WHITE);
        epaper.update();
        delay(2000);
    } else {
        // Normal refresh - just clear to white
        epaper.fillScreen(TFT_WHITE);
        epaper.update();
    }
    
    // Draw header BEFORE downloading image
    drawHeader();
    
    // Download and display PNG with offset
    uint8_t* imageData = nullptr;
    int imageSize = 0;
    
    if (downloadImage("http://devserver/wind/daywind.png", &imageData, &imageSize)) {
        Serial.println("[MAIN] Download successful");
        
        if (displayPNGStreaming(imageData, imageSize)) {
            Serial.println("[MAIN] Image streamed successfully");
        } else {
            Serial.println("[MAIN] Image streaming failed");
            epaper.fillScreen(TFT_WHITE);
            epaper.update();
        }
        
        free(imageData);
    } else {
        Serial.println("[MAIN] Download failed");
        delay(60000); // Wait 1 minute before trying again
    }
    
    Serial.printf("[MEM] Final free heap: %d bytes\n", ESP.getFreeHeap());
}*/

/*void loop() {
    static int refreshCount = 0;
    
    delay(300000); // 5 minute intervals (300,000ms)
    Serial.printf("[LOOP] Free heap: %d bytes\n", ESP.getFreeHeap());
    
    refreshCount++;
    
    // Force a full refresh every 12 updates (1 hour) to clear ghosting
    bool forceFullRefresh = (refreshCount % 12 == 0);
    
    if (forceFullRefresh) {
        Serial.println("[REFRESH] Performing full refresh to clear ghosting");
        
        // Full refresh cycle: white -> black -> white
        epaper.fillScreen(TFT_BLACK);
        epaper.update();
        delay(2000);
        
        epaper.fillScreen(TFT_WHITE);
        epaper.update();
        delay(2000);
    } else {
        // Normal refresh - just clear to white
        epaper.fillScreen(TFT_WHITE);
        epaper.update();
    }
    
    // Draw header BEFORE downloading image
    drawHeader();

    // Download and display new image
    uint8_t* imageData = nullptr;
    int imageSize = 0;
    
    if (downloadImage("http://devserver/wind/daywind.png", &imageData, &imageSize)) {
        Serial.println("[MAIN] Download successful");
        
        if (displayPNGStreaming(imageData, imageSize)) {
            Serial.println("[MAIN] Image streamed successfully");
        } else {
            Serial.println("[MAIN] Image streaming failed");
            epaper.fillScreen(TFT_WHITE);
            epaper.update();
        }
        
        free(imageData);
    } else {
        Serial.println("[MAIN] Download failed");
        delay(60000); // Wait 1 minute before trying again
    }
    
    Serial.printf("[MEM] Final free heap: %d bytes\n", ESP.getFreeHeap());
}*/

void loop() {
    static int refreshCount = 0;
    static unsigned long lastWiFiCheck = 0;

        // Check WiFi status periodically
    if (millis() - lastWiFiCheck > 60000) { // Check every minute
        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[WiFi] Status: %d, attempting reconnect...\n", WiFi.status());
            ensureWiFiConnected();
        }
        lastWiFiCheck = millis();
    }
    
    Serial.printf("[LOOP] Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("[WiFi] Status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    
    
    refreshCount++;
    
    // Force a full refresh every 12 updates (1 hour) to clear ghosting - 20mins
    bool forceFullRefresh = (refreshCount % 4 == 0);
    
    if (forceFullRefresh) {
        Serial.println("[REFRESH] Performing full refresh to clear ghosting");
        
        epaper.fillScreen(TFT_BLACK);
        epaper.update();
        delay(2000);
        
        epaper.fillScreen(TFT_WHITE);
        epaper.update();
        delay(2000);
    } else {
        epaper.fillScreen(TFT_WHITE);
        epaper.update();
    }
    
    // Draw header
    drawHeader();
    
    // Download and display first image (wind speed)
    uint8_t* imageData1 = nullptr;
    int imageSize1 = 0;
    
    // Position first image (wind speed)
    imageOffsetX = 0;
    imageOffsetY = 40;  // Below header
    
    //if (downloadImage("http://devserver/wind/daywind.png", &imageData1, &imageSize1)) {
    String windSpeedURL = buildURL(windSpeed);
    if (downloadImage(windSpeedURL.c_str(), &imageData1, &imageSize1)) {
        Serial.println("[MAIN] Wind speed download successful");
        
        if (displayPNGStreaming(imageData1, imageSize1)) {
            Serial.println("[MAIN] Wind speed image displayed successfully");
        } else {
            Serial.println("[MAIN] Wind speed image streaming failed");
        }
        
        free(imageData1);
    } else {
        Serial.println("[MAIN] Wind speed download failed");
    }
    
    // Download and display second image (wind direction)
    uint8_t* imageData2 = nullptr;
    int imageSize2 = 0;
    
    // Position second image next to first or below it
    // Option A: Side by side (if they fit)
    imageOffsetX = 420;  // Next to first image (350 * 1.2 = 420 pixels wide)
    imageOffsetY = 40;   // Same height as first image
    
    // Option B: Below first image
    // imageOffsetX = 0;
    // imageOffsetY = 40 + (180 * 1.2) + 10;  // Below first image + gap
    
    String windDirURL = buildURL(windDir);
    if (downloadImage(windDirURL.c_str(), &imageData2, &imageSize2)) {
        Serial.println("[MAIN] Wind direction download successful");
        
        if (displayPNGStreaming(imageData2, imageSize2)) {
            Serial.println("[MAIN] Wind direction image displayed successfully");
        } else {
            Serial.println("[MAIN] Wind direction image streaming failed");
        }
        
        free(imageData2);
    } else {
        Serial.println("[MAIN] Wind direction download failed");
    }
    
    Serial.printf("[MEM] Final free heap: %d bytes\n", ESP.getFreeHeap());
    delay(300000); // 5 minute intervals
    Serial.printf("[LOOP] Free heap: %d bytes\n", ESP.getFreeHeap());
}