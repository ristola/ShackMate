#include <ArduinoOTA.h>
#include <M5Unified.h>
#include <LittleFS.h>

#define LOGO_IMG_PATH "/ShackMateLogo.jpg"

auto &canvas = M5.Display; // Use the Display object directly

enum ScreenState
{
  SCREEN_BOOT,
  SCREEN_WIFI,
  SCREEN_DISCOVERY,
  SCREEN_POWER,
  SCREEN_RADIO
};

ScreenState currentScreen = SCREEN_BOOT;

void drawBootScreen()
{
  canvas.fillScreen(WHITE);
  canvas.setTextSize(4);
  canvas.setTextColor(BLACK);
  canvas.setCursor(50, 20);
  canvas.print("ShackMate Remote");
  if (LittleFS.exists(LOGO_IMG_PATH))
  {
    canvas.drawJpgFile(LOGO_IMG_PATH, 110, 160);
  }
  else
  {
    canvas.setCursor(110, 300);
    canvas.print("LOGO MISSING");
  }
  canvas.setTextSize(3);
  canvas.setCursor(150, 850);
  canvas.print("Joining WiFi...");
}

void drawDiscoveryScreen()
{
  canvas.fillScreen(WHITE);
  canvas.setTextSize(4);
  canvas.setCursor(110, 400);
  canvas.print("Looking for Devices ...");
}

void drawPowerScreen()
{
  canvas.fillScreen(WHITE);
  canvas.setTextSize(4);
  canvas.setCursor(80, 40);
  canvas.print("POWER MANAGEMENT");

  canvas.setTextSize(3);
  canvas.setCursor(40, 180);
  canvas.print("ASTRON 50 AMP SUPPLY");
  canvas.drawRect(20, 160, 420, 60);

  canvas.setCursor(40, 280);
  canvas.print("Outlet 2");
  canvas.drawRect(20, 260, 420, 60);

  canvas.setCursor(40, 380);
  canvas.print("IC-7300");
  canvas.drawRect(20, 360, 200, 60);

  canvas.setCursor(260, 380);
  canvas.print("IC-9700");
  canvas.drawRect(240, 360, 200, 60);

  canvas.setTextSize(2);
  canvas.setCursor(280, 900);
  canvas.print("Tap to RADIO");
}

void drawRadioScreen()
{
  canvas.fillScreen(WHITE);
  canvas.setTextSize(4);
  canvas.setCursor(80, 40);
  canvas.print("Memory Keyer");

  int x0 = 20, y0 = 120, w = 220, h = 70, gap = 30;
  canvas.setTextSize(3);

  canvas.setCursor(x0 + 25, y0 + 20);
  canvas.print("Voice");
  canvas.drawRect(x0, y0, w, h);

  canvas.setCursor(x0 + w + gap + 50, y0 + 20);
  canvas.print("CW");
  canvas.drawRect(x0 + w + gap, y0, w, h);

  canvas.setCursor(x0 + 2 * (w + gap) + 25, y0 + 20);
  canvas.print("RTTY");
  canvas.drawRect(x0 + 2 * (w + gap), y0, w, h);

  int btnW = 140, btnH = 60, btnGapX = 35, btnGapY = 28;
  int startX = 40, startY = 230;
  for (int row = 0; row < 2; ++row)
  {
    for (int col = 0; col < 4; ++col)
    {
      int idx = row * 4 + col + 1;
      String label = "M" + String(idx);
      int bx = startX + col * (btnW + btnGapX);
      int by = startY + row * (btnH + btnGapY);
      canvas.drawRect(bx, by, btnW, btnH);
      canvas.setCursor(bx + 40, by + 15);
      canvas.print(label);
    }
  }

  canvas.setTextSize(2);
  canvas.setCursor(300, 900);
  canvas.print("Tap to POWER");
}

void setup()
{
  M5.begin();
  M5.Display.setRotation(1);
  Serial.begin(115200);
  M5.Touch.begin(&M5.Display); // Provide display reference

  if (!LittleFS.begin())
  {
    Serial.println("LittleFS Mount Failed");
  }
  else
  {
    Serial.println("LittleFS ready.");
  }

  drawBootScreen();
  delay(1800);

  currentScreen = SCREEN_WIFI;
  drawDiscoveryScreen();
  delay(1200);

  currentScreen = SCREEN_POWER;
  drawPowerScreen();
}

void loop()
{
  M5.update();

  static bool touchPrev = false;
  bool touchNow = (M5.Touch.getCount() > 0);

  // Only trigger on *new* touch
  if (touchNow && !touchPrev)
  {
    if (currentScreen == SCREEN_POWER)
    {
      currentScreen = SCREEN_RADIO;
      drawRadioScreen();
    }
    else if (currentScreen == SCREEN_RADIO)
    {
      currentScreen = SCREEN_POWER;
      drawPowerScreen();
    }
  }
  touchPrev = touchNow;

  delay(10);
}