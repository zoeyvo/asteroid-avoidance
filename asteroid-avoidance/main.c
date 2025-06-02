//*****************************************************************************
// main.c - Asteroid Avoidance Survival Game
//*****************************************************************************

// ========================= INCLUDES =========================

// Standard includes
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

// Driverlib includes
#include "hw_types.h"
#include "hw_common_reg.h"
#include "hw_ints.h"
#include "spi.h"
#include "rom.h"
#include "rom_MAP.h"
#include "utils.h"
#include "prcm.h"
#include "uart.h"
#include "interrupt.h"
#include "hw_nvic.h"
#include "hw_memmap.h"
#include "hw_apps_rcm.h"
#include "gpio.h"

// Common interface includes
#include "uart_if.h"
#include "gpio_if.h"
#include "i2c_if.h"
#include "common.h"

// Simplelink includes
#include "simplelink.h"

// Custom includes
#include "utils/network_utils.h"

// Timing interrupt
#include "systick.h"
#include "interrupt.h"
#include "timer.h"
#include "timer_if.h"

// SysConfig
#include "pin_mux_config.h"

// Adafruit Graphics
#include "oled_test.h"
#include "Adafruit_GFX.h"
#include "Adafruit_SSD1351.h"
#include "glcdfont.h"

// ========================= DEFINES =========================

#define BLACK                   0x0000
#define WHITE                   0xFFFF
#define PASTEL_RED              0xFBB2

// STUFF FOR TARGETS
#define MAX_ASTEROIDS       5  // Maximum number of asteroids possible
#define ASTEROID_MIN_RADIUS 6
#define ASTEROID_MAX_RADIUS 12

// Score milestone system for dynamic asteroid spawning
#define SCORE_MILESTONE_BASE 100    // First milestone at 100 points
#define MAX_ACTIVE_ASTEROIDS 5      // Maximum asteroids that can be active simultaneously

// Asteroid speed definitions - 4 fixed speed tiers (all slower than before)
#define ASTEROID_SPEED_SLOW     1   // 2 pixels per frame
#define ASTEROID_SPEED_MEDIUM   2   // 3 pixels per frame
#define ASTEROID_SPEED_FAST     3   // 4 pixels per frame
#define ASTEROID_SPEED_FASTEST  4   // 5 pixels per frame

#define SPI_IF_BIT_RATE  20000000
#define UART_BAUD_RATE   115200
#define TR_BUFF_SIZE     100
#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    128
#define MAX_MSG_LEN      128

#define SYSCLKFREQ       80000000ULL
#define TICKS_TO_US(ticks) \
    ((((ticks) / SYSCLKFREQ) * 1000000ULL) + \
    ((((ticks) % SYSCLKFREQ) * 1000000ULL) / SYSCLKFREQ))
#define US_TO_TICKS(us) ((SYSCLKFREQ / 1000000ULL) * (us))
#define SYSTICK_RELOAD_VAL 3200000UL

#define SHORT_PULSE_MIN 400
#define SHORT_PULSE_MAX 600
#define LONG_PULSE_MIN  1500
#define LONG_PULSE_MAX  1700

#define DATE            2    /* Current Date */
#define MONTH           6     /* Month 1-12 */
#define YEAR            2025  /* Current year */
#define HOUR            4    /* Time - hours */
#define MINUTE          27    /* Time - minutes */
#define SECOND          0     /* Time - seconds */

#define APPLICATION_NAME      "SSL"
#define APPLICATION_VERSION   "SQ24"
#define SERVER_NAME           "a1m8o1coxrb26a-ats.iot.us-east-2.amazonaws.com"
#define GOOGLE_DST_PORT       8443
#define POSTHEADER "POST /things/CC3200/shadow HTTP/1.1\r\n"
#define HOSTHEADER "Host: a1m8o1coxrb26a-ats.iot.us-east-2.amazonaws.com\r\n"
#define CHEADER "Connection: Keep-Alive\r\n"
#define CTHEADER "Content-Type: application/json; charset=utf-8\r\n"
#define CLHEADER1 "Content-Length: "
#define CLHEADER2 "\r\n\r\n"

#define FOREVER                 1
#define FAILURE                 -1
#define SUCCESS                 0
#define RETERR_IF_TRUE(condition) {if(condition) return FAILURE;}
#define RET_IF_ERR(Func)          {int iRetVal = (Func); \
                                   if (SUCCESS != iRetVal) \
                                     return  iRetVal;}
#define MAX_ASTEROID_SPEED 0.1 // Much slower asteroids (was 1)

// Frame rate control
#define TARGET_FPS 45                    // Target 60 FPS for smooth gameplay
#define FRAME_DELAY_TICKS (SYSCLKFREQ / TARGET_FPS)  // Ticks per frame

// ========================= TYPEDEFS =========================

typedef struct {
    int x, y;
    int dx, dy;
    int radius;
    int sides;
    int speed; // used for scoring
} Asteroid;

typedef struct PinSetting {
    unsigned long port;
    unsigned int pin;
} PinSetting;

typedef enum {
    GAME_STATE_START_SCREEN,
    GAME_STATE_PLAYING,
    GAME_STATE_GAME_OVER,
    GAME_STATE_WAITING_RESTART
} GameState;

// ========================= GLOBAL VARIABLES =========================

Asteroid asteroids[MAX_ASTEROIDS];

#define NUM_ASTEROID_SLOTS      MAX_ACTIVE_ASTEROIDS
static int asteroid_slot_used[NUM_ASTEROID_SLOTS] = {0};
static int asteroid_slot_x[NUM_ASTEROID_SLOTS] = {0};

// OLED/SPI variables
char messageBuffer[MAX_MSG_LEN];
char displayBuffer[MAX_MSG_LEN] = {0};
int bufferIndex = 0;

// UART buffers
char MessageTx[MAX_MSG_LEN];
int msgIndex = 0;

// Game state variables
int player_lives = 3;
int player_score = 0;
int ship_x = SCREEN_WIDTH / 2;
int ship_y = SCREEN_HEIGHT - 32;  // 32 pixels above bottom of screen
int ship_size = 10;
int x_speed = 0;
int y_speed = 0;

// Time-based scoring variables
unsigned long game_start_time = 0;

// Dynamic asteroid spawning variables
int current_num_asteroids = 1;     // Current number of active asteroids
int last_milestone_reached = 0;    // Track last score milestone reached
int next_milestone = SCORE_MILESTONE_BASE;  // Next milestone to trigger spawning

// Game state management for continuous IR loop
volatile GameState current_game_state = GAME_STATE_START_SCREEN;

// IR/Decoding/Systick/Interrupts variables
volatile unsigned long IR_intcount;
volatile unsigned char IR_intflag;
volatile int edge = 1;
volatile int bit_count = 0;
volatile uint64_t decoded_sequence = 0;
volatile uint64_t delta_ticks = 0;
volatile uint64_t delta_us = 0;

// Multi-tap messaging state
volatile int curButton = -1;
volatile int prevButton = -1;
int lastPressTime = 0;
volatile int pressCount = 0;
int currentTime = 0;
// Alphanumeric key mapping for each button (like old phone keypads)
const char* keyMap[12] = {
    " 0",        // 0
    "1",        // 1
    "ABC2",    // 2
    "DEF3",    // 3
    "GHI4",    // 4
    "JKL5",    // 5
    "MNO6",    // 6
    "PQRS7",   // 7
    "TUV8",    // 8
    "WXYZ9",   // 9
    "\b",      //  MUTE
    "\n",       // LAST
};

// Timeout Interrupt
static volatile unsigned long g_ulSysTickValue;
static volatile unsigned long g_ulBase;
static volatile unsigned long g_ulRefBase;
static volatile unsigned long g_ulRefTimerInts = 0;
static volatile unsigned long g_ulIntClearVector;
unsigned long g_ulTimerInts;
static const PinSetting IR_SIGNAL = { .port = GPIOA0_BASE, .pin = 0x80};

// AWS/IoT buffers
char awsMessage[MAX_MSG_LEN];
char DATA1[MAX_MSG_LEN];  // Buffer for the JSON string
long lRetVal = -1;
#if defined(ccs)
extern void (* const g_pfnVectors[])(void);
#endif
#if defined(ewarm)
extern uVectorEntry __vector_table;
#endif

// ========================= FUNCTION PROTOTYPES =========================
// --- Initialization ---
void gameInit();
void boardInit();
void pinmuxInit();
void uartInit();
void spiInit();
void adafruitInit();
void i2cInit();
void systickInit();
void interruptInit();
void terminalInit();
void awsInit();
void varInit();
// --- Main Game Loop ---
void startGame();
void updateState();
void endGame();
// --- Game Logic ---
void renderAsteroids();
void drawUI();
void checkCollisions();
void updatePositions();
void showGameOverScreen(int score, int isHighScore);
void printOLED(const char msg[], int x, int y, unsigned int color);
void drawDividerLine();
void MasterMain();
void onButtonPress(int button);
void matchSequence(uint32_t decodedSequence);
static void GPIOA0IntHandler(void);
int decodePulse(int timeElapsed);
static void SysTickInit(void);
static inline void SysTickReset(void);
void TimerBaseIntHandler(void);
int DisplayBuffer(unsigned char *pucDataBuf, unsigned char ucLen);
int ProcessReadRegCommand(char *pcInpString);
int ParseNProcessCmd(char *pcCmdBuffer);
void buildJson();
static int set_time();
void awsMsg();
int getHighScoreFromAWS();
static int http_get(int iTLSSockID);
static int http_post(int);
static void BoardInit(void);
void drawShip(int x, int y, int size, unsigned int color);
void drawAsteroidPolygon(int cx, int cy, int radius, int sides, unsigned int color);
void spawnAsteroid(Asteroid* t);
void initAsteroids();
void drawGameObjects(int ship_x, int ship_y, int ship_size, int prev_ship_x, int prev_ship_y);
// --- Dynamic Asteroid Spawning System ---
void checkScoreMilestones();
int spawnNewAsteroidSafely();
// --- Accelerometer Functions ---
int readAccelAxis(char axis);
int readAccelX();
int readAccelY();
void updateShipFromAccel();
// --- Efficient Rendering ---
void efficientRender(int prev_ship_x, int prev_ship_y);
void eraseShip(int x, int y, int size);
void eraseAsteroid(int index);

// ========================= FUNCTION IMPLEMENTATIONS =========================
// --- Initialization ---
void gameInit() {
    Report("=== INITIALIZING GAME SYSTEMS ===\r\n");
    boardInit();
    pinmuxInit();
    uartInit();
    spiInit();
    adafruitInit();
    i2cInit();
    systickInit();
    interruptInit();
    terminalInit();
    awsInit();
    varInit();
}

void boardInit() { BoardInit(); }
void pinmuxInit() { PinMuxConfig(); }
void uartInit() { InitTerm(); }
void spiInit() { MasterMain(); }
void adafruitInit() { Adafruit_Init(); fillScreen(BLACK); }
void systickInit() { SysTickInit(); }
void terminalInit() { InitTerm(); ClearTerm(); }
void awsInit() {
    Report("Initializing AWS IoT connection...\r\n");
    g_app_config.host = SERVER_NAME;
    g_app_config.port = GOOGLE_DST_PORT;

    lRetVal = connectToAccessPoint();
    if (lRetVal < 0) {
        Report("Failed to connect to access point: %d\r\n", lRetVal);
        return;
    }

    lRetVal = set_time();
    if (lRetVal < 0) {
        Report("Failed to set time: %d\r\n", lRetVal);
        return;
    }

    lRetVal = tls_connect();
    if (lRetVal < 0) {
        Report("Failed to establish TLS connection: %d\r\n", lRetVal);
        return;
    }

    http_get(lRetVal); // get aws highscore
}
void varInit() {
    // Reset all game state variables at the start of each game
    player_lives = 3;
    player_score = 0;
    ship_x = SCREEN_WIDTH / 2;
    ship_y = SCREEN_HEIGHT - 32;  // 32 pixels above bottom of screen
    ship_size = 10;
    x_speed = 0;
    y_speed = 0;

    initAsteroids();
    Report("Game variables reset complete\r\n");
}

// ========================= MAIN =========================
// Main entry point for Asteroid Avoidance survival game
int main() {
    Report("=== ASTEROID AVOIDANCE STARTING ===\r\n");
    gameInit();

    // Display initial start screen
    startGame();

    int bit_value = 0;
    unsigned long last_frame_time = 0;  // Track last frame time for FPS control

    while (1) {
        // Process IR input first (highest priority)
        if (IR_intflag) {
            IR_intflag = 0;
            if (delta_ticks > 0) {
                delta_us = TICKS_TO_US(delta_ticks);
                bit_value = decodePulse(delta_us);
                if (bit_value == 1 || bit_value == 0) {
                    bit_count++;
                    decoded_sequence = (decoded_sequence << 1) | bit_value;
                    if (bit_count == 32) {
                        Report("Received: 0x%08X\r\n", (unsigned int)decoded_sequence);
                        matchSequence((uint32_t)decoded_sequence);
                        decoded_sequence = 0;
                        bit_count = 0;
                    }
                } else {
                    // Invalid pulse, reset sequence
                    if (bit_count > 0) {
                        Report("Received: Invalid Signal\r\n");
                    }
                    decoded_sequence = 0;
                    bit_count = 0;
                }
            }
        }

        // Frame rate limited game updates when playing
        if (current_game_state == GAME_STATE_PLAYING) {
            unsigned long current_time = SysTickValueGet();
            unsigned long elapsed_ticks;

            // Calculate elapsed time since last frame (handle wraparound)
            if (current_time <= last_frame_time) {
                elapsed_ticks = last_frame_time - current_time;
            } else {
                elapsed_ticks = (SYSTICK_RELOAD_VAL - current_time) + last_frame_time;
            }

            // Only update if enough time has passed for target FPS
            if (elapsed_ticks >= FRAME_DELAY_TICKS) {
                static int prev_ship_x = 0, prev_ship_y = 0;

                // Store previous positions for efficient redrawing
                prev_ship_x = ship_x;
                prev_ship_y = ship_y;

                updatePositions();
                checkCollisions();

                // Efficient rendering with position tracking
                efficientRender(prev_ship_x, prev_ship_y);

                // Update frame timing
                last_frame_time = current_time;

                // If player_lives == 0, transition to game over
                if (player_lives == 0) {
                    current_game_state = GAME_STATE_GAME_OVER;
                    endGame();
                }
            }
        }
    }
}

// ========================= INITIALIZATION SECTION =========================

void BoardInit(void)
{
#ifndef USE_TIRTOS
#if defined(ccs)
    MAP_IntVTableBaseSet((unsigned long)&g_pfnVectors[0]);
#endif
#if defined(ewarm)
    MAP_IntVTableBaseSet((unsigned long)&__vector_table);
#endif
#endif
    MAP_IntMasterEnable();
    MAP_IntEnable(FAULT_SYSTICK);
    PRCMCC3200MCUInit();
}

// ========================= GAME LOOP SECTION =========================

void startGame() {
    fillScreen(BLACK);

    // Get current high score from AWS for display
    int awsHighScore = getHighScoreFromAWS();
    char highScoreText[32];
    sprintf(highScoreText, "High Score: %d", awsHighScore);    printOLED("ASTEROID AVOIDANCE", (128 - strlen("ASTEROID AVOIDANCE") * 6) / 2, 128/2 - 24, GREEN);
    printOLED(highScoreText, (128 - strlen(highScoreText) * 6) / 2, 128/2, GREEN);
    printOLED("Press MUTE to start", (128 - strlen("Press MUTE to start") * 6) / 2, 128/2 + 24, WHITE);
    printOLED("Tilt left/right to move", (128 - strlen("Tilt left/right to move") * 6) / 2, 128/2 + 36, WHITE);

    Report("=== [STARTING GAME] ===\r\n");
}

void updateState() {
    fillScreen(BLACK);
    // On first entry: draw ship, wait 3s, draw asteroids
    drawShip(ship_x, ship_y, ship_size, WHITE);
    drawUI();
    MAP_UtilsDelay(16000000); // 3s delay
    renderAsteroids();

    int prev_ship_x = ship_x;
    int prev_ship_y = ship_y;

    while (current_game_state == GAME_STATE_PLAYING) {
        // No longer need to process IR input here - it's handled in continuous loop

        // Store previous positions for efficient redrawing
        prev_ship_x = ship_x;
        prev_ship_y = ship_y;

        updatePositions();
        checkCollisions();

        // Efficient rendering with position tracking
        efficientRender(prev_ship_x, prev_ship_y);

        // If player_lives == 0, transition to game over
        if (player_lives == 0) {
            current_game_state = GAME_STATE_GAME_OVER;
            endGame();
            break;
        }

    }
}

void endGame() {
    Report("Game ended. Player score: %d\r\n", player_score);

    // Get current high score from AWS
    int awsHighScore = getHighScoreFromAWS();
    int isHighScore = 0;

    // Only post to AWS if the current score is higher than the AWS high score
    if (player_score > awsHighScore) {
        isHighScore = 1;
        Report("New high score achieved: %d (previous AWS high score: %d)\r\n", player_score, awsHighScore);

        // POST new high score to AWS (key: "highscore:")
        snprintf(MessageTx, sizeof(MessageTx), "%d", player_score);
        buildJson();
        awsMsg();
        Report("High score posted to AWS\r\n");
    } else {
        isHighScore = 0;
        Report("Final score: %d (Current AWS high score: %d) - No new high score\r\n", player_score, awsHighScore);
    }

    showGameOverScreen(player_score, isHighScore);

    // Set state to waiting for restart - IR loop will handle button press
    current_game_state = GAME_STATE_WAITING_RESTART;
    Report("Game over. Waiting for any button press to restart...\r\n");
}

// --- Asteroid spawn slots for evenly spaced positions ---
void initAsteroidSlots() {
    int i;
    int slot_width = SCREEN_WIDTH / NUM_ASTEROID_SLOTS;
    for (i = 0; i < NUM_ASTEROID_SLOTS; i++) {
        asteroid_slot_used[i] = 0;
        asteroid_slot_x[i] = (slot_width / 2) + i * slot_width;
    }
}

int getFreeAsteroidSlot() {
    // Collect all free slots
    int free_slots[NUM_ASTEROID_SLOTS];
    int count = 0;
    int i;
    for (i = 0; i < NUM_ASTEROID_SLOTS; i++) {
        if (!asteroid_slot_used[i]) {
            free_slots[count++] = i;
        }
    }
    if (count == 0) return -1;
    // Shuffle free_slots array to randomize order
    for (i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = free_slots[i];
        free_slots[i] = free_slots[j];
        free_slots[j] = temp;
    }
    // Return the first slot in the shuffled array
    return free_slots[0];
}

void setAsteroidSlotUsed(int slot, int used) {
    if (slot >= 0 && slot < NUM_ASTEROID_SLOTS) asteroid_slot_used[slot] = used;
}

void spawnAsteroidInSlot(Asteroid* t, int slot) {
    // Use arrays instead of switch statements for size and speed
    static const int radius_options[] = {6, 8, 10, 12};
    static const int speed_options[] = {ASTEROID_SPEED_SLOW, ASTEROID_SPEED_MEDIUM, ASTEROID_SPEED_FAST, ASTEROID_SPEED_FASTEST};

    int r = radius_options[rand() % 4];
    int dy = speed_options[rand() % 4];
    int y = -r - 10;
    int x = asteroid_slot_x[slot];
    int dx = 0;
    t->x = x;
    t->y = y;
    t->dx = dx;
    t->dy = dy;
    t->radius = r;
    t->sides = 4;
    t->speed = dy;
    setAsteroidSlotUsed(slot, 1);
    Report("Spawned asteroid in slot %d at x=%d\n", slot, x);
}

int spawnNewAsteroidSafely() {
    int slot = getFreeAsteroidSlot();
    if (slot == -1) {
        Report("No available asteroid slots for spawning\n");
        return 0;
    }
    int i;
    int asteroid_idx = -1;
    for (i = 0; i < MAX_ASTEROIDS; i++) {
        if (asteroids[i].radius == 0) {
            asteroid_idx = i;
            break;
        }
    }
    if (asteroid_idx == -1) return 0;
    spawnAsteroidInSlot(&asteroids[asteroid_idx], slot);
    return 1;
}

void freeAsteroidSlotForAsteroid(Asteroid* t) {
    int i;
    for (i = 0; i < NUM_ASTEROID_SLOTS; i++) {
        if (asteroid_slot_x[i] == t->x) {
            setAsteroidSlotUsed(i, 0);
            break;
        }
    }
}

// ========================= HELPER FUNCTION IMPLEMENTATIONS =========================

// Draw all asteroids in white
void renderAsteroids() {
    int i;
    for (i = 0; i < current_num_asteroids; i++) {
        if (asteroids[i].radius == 0) continue; // Skip uninitialized
        drawAsteroidPolygon(asteroids[i].x, asteroids[i].y, asteroids[i].radius, asteroids[i].sides, PASTEL_RED);
    }
}

// Draw score, lives, and time in white at fixed positions
void drawUI() {
    extern int player_score, player_lives;
    char score_text[16];
    char lives_text[16];

    sprintf(score_text, "SCORE:%d", player_score);
    sprintf(lives_text, "LIVES:%d", player_lives);

    // Determine color based on remaining lives
    unsigned int livesColor = WHITE;
    if (player_lives == 3) {
        livesColor = GREEN;
    } else if (player_lives == 2) {
        livesColor = YELLOW;
    } else if (player_lives == 1) {
        livesColor = RED;
    }

    // Print score and time in white
    printOLED(score_text, 2, 2, GREEN); // white

    // Print lives with dynamic color
    int c;
    const int charWidth = 6;
    for (c = 0; c < strlen(lives_text); c++) {
        drawChar(SCREEN_WIDTH - 8 * charWidth + (c * charWidth), 2, lives_text[c], livesColor, BLACK, 1);
    }
}

// Check for collisions: ship vs asteroids only (no lasers)
void checkCollisions() {
    extern int ship_x, ship_y, ship_size, player_lives;
    int i;

    for (i = 0; i < current_num_asteroids; i++) {
        // Skip uninitialized asteroids
        if (asteroids[i].radius == 0) continue;

        // Ship vs asteroid collision - using overlapping area detection
        int dx = ship_x - asteroids[i].x;
        int dy = ship_y - asteroids[i].y;
        int dist2 = dx*dx + dy*dy;

        // Calculate collision boundaries - ship radius + asteroid radius
        int ship_radius = ship_size / 2;
        int asteroid_radius = asteroids[i].radius;
        int min_dist = ship_radius + asteroid_radius - 1; // Allow 1 pixel overlap for better feel

        // Check if objects are overlapping (collision detected)
        if (dist2 < min_dist*min_dist) {
            player_lives--;            Report("COLLISION! Ship (radius %d) overlapped with asteroid %d (radius %d). Lives remaining: %d\r\n",
                   ship_radius, i, asteroid_radius, player_lives);

            // Visual feedback for collision
            fillScreen(RED);
            MAP_UtilsDelay(10000000); // Brief flash
            fillScreen(BLACK);            if (player_lives > 0) {
                Report("Respawning ship and resetting round...\r\n");
                // Reset ship position to 32 pixels above bottom
                ship_x = SCREEN_WIDTH / 2;
                ship_y = SCREEN_HEIGHT - 32;  // 32 pixels above bottom of screen

                x_speed = 0;
                y_speed = 0;                // Respawn all asteroids
                initAsteroids();
                drawShip(ship_x, ship_y, ship_size, WHITE);
                drawUI();
                MAP_UtilsDelay(10000000); // 0.75s delay for recovery
                renderAsteroids();
            } else {
                Report("GAME OVER - No lives remaining!\r\n");
            }
            return;
        }
    }
}

// Update positions of ship and asteroids with horizontal-only movement
void updatePositions() {
    extern int ship_x, ship_y, ship_size, x_speed, y_speed;
    updateShipFromAccel();
    ship_x += x_speed;
    // ship_y += y_speed / 1.25; // Removed - ship no longer moves vertically
    if (ship_x < 0) {
        ship_x = SCREEN_WIDTH - 1;
        Report("Ship wrapped from left to right side (x=%d)\r\n", ship_x);
    }
    if (ship_x >= SCREEN_WIDTH) {
        ship_x = 0;
        Report("Ship wrapped from right to left side (x=%d)\r\n", ship_x);
    }
    // Y boundary checks removed - ship stays at fixed Y position
    int i;
    for (i = 0; i < current_num_asteroids; i++) {
        if (asteroids[i].radius == 0) continue;
        asteroids[i].y += asteroids[i].dy;
        if (asteroids[i].y - asteroids[i].radius > SCREEN_HEIGHT + 32) {
            freeAsteroidSlotForAsteroid(&asteroids[i]);
            drawAsteroidPolygon(asteroids[i].x, asteroids[i].y, asteroids[i].radius, asteroids[i].sides, BLACK);
            int asteroid_points = asteroids[i].radius * asteroids[i].speed;
            player_score += asteroid_points;
            Report("Asteroid %d completely off bottom (top edge at y=%d), awarding %d points (radius %d * speed %d). Total score: %d\r\n",
                   i, asteroids[i].y - asteroids[i].radius, asteroid_points, asteroids[i].radius, asteroids[i].speed, player_score);
            int slot = getFreeAsteroidSlot();
            if (slot != -1) {
                spawnAsteroidInSlot(&asteroids[i], slot);
            }
        }
    }
    checkScoreMilestones();
}

// ========================= EFFICIENT RENDERING SECTION =========================
// Efficient rendering system that only redraws changed objects with reduced frequency
void efficientRender(int prev_ship_x, int prev_ship_y) {    // Only redraw ship if it moved
    if (ship_x != prev_ship_x || ship_y != prev_ship_y) {
        eraseShip(prev_ship_x, prev_ship_y, ship_size);
        drawShip(ship_x, ship_y, ship_size, WHITE);
    }// Redraw asteroids (they're always moving)
    int i;
    for (i = 0; i < current_num_asteroids; i++) {
        if (asteroids[i].radius == 0) continue; // Skip uninitialized
        eraseAsteroid(i);
    }
    renderAsteroids();

    // Check if any asteroid is near UI areas (top part of screen)
    static int ui_counter = 0;
    int ui_area_threatened = 0;

    // Check if any asteroid is in the top portion of screen where UI is displayed
    for (i = 0; i < current_num_asteroids; i++) {
        if (asteroids[i].radius == 0) continue; // Skip uninitialized
        if (asteroids[i].y - asteroids[i].radius <= 20) { // Top 20 pixels contain UI
            ui_area_threatened = 1;
            break;
        }
    }

    // Update UI more frequently when asteroids are near UI area, less frequently otherwise
    ui_counter++;
    if (ui_area_threatened && ui_counter >= 3) {
        // Redraw UI every 3 frames when asteroids are near to prevent erasure
        drawUI();
        ui_counter = 0;
    } else if (!ui_area_threatened && ui_counter >= 30) {
        // Update UI every 30 frames when no asteroids nearby (every 0.5 seconds at 60 FPS)
        drawUI();
        ui_counter = 0;
    }
}

// Erase ship at previous position
void eraseShip(int x, int y, int size) {
    drawShip(x, y, size, BLACK);
}

// Erase asteroid at previous position
void eraseAsteroid(int index) {
    // Calculate previous position based on velocity
    int prev_x = asteroids[index].x - asteroids[index].dx;
    int prev_y = asteroids[index].y - asteroids[index].dy;
    drawAsteroidPolygon(prev_x, prev_y, asteroids[index].radius, asteroids[index].sides, BLACK);
}

// Show GAME OVER screen and high score info
void showGameOverScreen(int score, int isHighScore) {
    fillScreen(BLACK);

    printOLED("GAME OVER", (SCREEN_WIDTH - strlen("GAME OVER") * 6) / 2, SCREEN_HEIGHT / 2 - 36, RED);

    char score_text[32];
    sprintf(score_text, "Final Score: %d", score);
    printOLED(score_text, (SCREEN_WIDTH - strlen(score_text) * 6) / 2, SCREEN_HEIGHT / 2 - 12, GREEN);

    // High score notification
    if (isHighScore) {
        printOLED("NEW HIGH SCORE!", (SCREEN_WIDTH - strlen("NEW HIGH SCORE!") * 6) / 2, SCREEN_HEIGHT / 2, GREEN);
    } else {
        printOLED("Try again!", (SCREEN_WIDTH - strlen("Try again!") * 6) / 2, SCREEN_HEIGHT / 2, WHITE);
    }

    printOLED("Press any button", (SCREEN_WIDTH - strlen("Press any button") * 6) / 2, SCREEN_HEIGHT / 2 + 24, WHITE);
    printOLED("to play again", (SCREEN_WIDTH - strlen("to play again") * 6) / 2, SCREEN_HEIGHT / 2 + 36, WHITE);
}

// ========================= IR/DECODING/SYSTICK/INTERRUPTS SECTION =========================
// Handle button press event (game input/shooting) with continuous IR monitoring
void onButtonPress(int button) {
    // Define descriptive button names
    const char* buttonNames[12] = {
        "0", "1", "2 (ABC)", "3 (DEF)", "4 (GHI)", "5 (JKL)",
        "6 (MNO)", "7 (PQRS)", "8 (TUV)", "9 (WXYZ)", "MUTE", "LAST"
    };

    if (button < 0 || button >= 12) {
        Report("Invalid button press detected: %d\r\n", button);
        return;
    }

    Report("IR Button pressed: %s (Button %d) - Game State: %d\r\n",
           buttonNames[button], button, current_game_state);

    switch (current_game_state) {

        case GAME_STATE_START_SCREEN:
            if (button == 10) { // MUTE to start game
                Report("Starting new game from start screen\r\n");
                varInit();
                current_game_state = GAME_STATE_PLAYING;                fillScreen(BLACK);
                drawShip(ship_x, ship_y, ship_size, WHITE);
                drawUI();
                MAP_UtilsDelay(16000000); // ~3s delay for ship display
                renderAsteroids();

                Report("Game started - entering gameplay state\r\n");
            }
            break;        case GAME_STATE_PLAYING:
            if (button >= 1 && button <= 9) {
                Report("Number button pressed during gameplay (no action in survival mode)\r\n");
            } else if (button == 10) { // MUTE
                Report("MUTE button during gameplay - could implement pause\r\n");
            } else if (button == 11) { // LAST - Emergency stop
                x_speed = y_speed = 0;
                Report("Emergency stop - All ship movement stopped during gameplay\r\n");
            } else if (button != 0) {
                Report("Unhandled button during gameplay: %d\r\n", button);
            }
            break;

        case GAME_STATE_GAME_OVER:
        case GAME_STATE_WAITING_RESTART:
            // Any button restarts the game
            Report("Button pressed during game over - returning to start screen\r\n");
            current_game_state = GAME_STATE_START_SCREEN;
            startGame();
            break;

        default:
            Report("Unknown game state: %d\r\n", current_game_state);
            break;
    }
}


// Match decoded IR sequence to button value
void matchSequence(uint32_t decodedSequence) {
    Report("IR sequence decoded: 0x%08X\r\n", (unsigned int)decodedSequence);

    // Lookup table for IR sequences - more compact than switch
    static const struct { uint32_t sequence; int button; } ir_map[] = {
        {0xDF20F708, 0}, {0xDF207788, 1}, {0xDF20B748, 2}, {0xDF2037C8, 3},
        {0xDF20D728, 4}, {0xDF2057A8, 5}, {0xDF209768, 6}, {0xDF2017E8, 7},
        {0xDF20E718, 8}, {0xDF206798, 9}, {0xDF20AF50, 10}, {0xDF20A758, 11}    };

    curButton = -1; // Default to invalid
    int i;
    for (i = 0; i < sizeof(ir_map)/sizeof(ir_map[0]); i++) {
        if (ir_map[i].sequence == decodedSequence) {
            curButton = ir_map[i].button;
            break;
        }
    }

    onButtonPress(curButton);
}

// GPIO interrupt handler for IR signal edge detection
static void GPIOA0IntHandler(void) {
    IR_intflag = 1;
    if (edge == 1) {    // Rising edge: start of pulse
        SysTickReset();
        edge = 0;
        MAP_GPIOIntTypeSet(IR_SIGNAL.port, IR_SIGNAL.pin, GPIO_FALLING_EDGE);
    } else {            // Falling edge: end of pulse
        delta_ticks = SYSTICK_RELOAD_VAL - SysTickValueGet();
        edge = 1;
        MAP_GPIOIntTypeSet(IR_SIGNAL.port, IR_SIGNAL.pin, GPIO_RISING_EDGE);
    }
    unsigned long ulStatus = MAP_GPIOIntStatus(GPIOA0_BASE, true);
    MAP_GPIOIntClear(GPIOA0_BASE, ulStatus);
}

// Decode IR pulse width to bit value
int decodePulse(int timeElapsed){
    if(SHORT_PULSE_MIN < timeElapsed && timeElapsed < SHORT_PULSE_MAX){
        return 1; // short pulse
    }
    else if (LONG_PULSE_MIN < timeElapsed && timeElapsed < LONG_PULSE_MAX){
        return 0; // long pulse
    }
    else {
        return -1;
    }
}

// Initialize SysTick timer
static void SysTickInit(void) {
    MAP_SysTickPeriodSet(SYSTICK_RELOAD_VAL);
    MAP_SysTickIntDisable();
    MAP_SysTickEnable();
}

// Reset SysTick timer and timing variables
static inline void SysTickReset(void) {
    HWREG(NVIC_ST_CURRENT) = 1;
    delta_ticks = 0;
    delta_us = 0;
}

// Timer interrupt handler for multi-tap input timeout
void TimerBaseIntHandler(void) {
    Timer_IF_InterruptClear(g_ulBase);
    if (pressCount > 0 && prevButton >= 0 && bufferIndex < sizeof(displayBuffer) - 1) {
        int btnChoices = strlen(keyMap[prevButton]);
        if (btnChoices > 0) {
            char c = keyMap[prevButton][(pressCount - 1) % btnChoices];
            displayBuffer[bufferIndex - 1] = c;
            printOLED(displayBuffer, 0, 98, WHITE); // Display at bottom of screen (y=98)
            Report("%c", c);
            MessageTx[msgIndex++] = c;
            MessageTx[msgIndex] = '\0';
            pressCount = 0;
            prevButton = -1;
        }
    } else {
        pressCount = 0;
        prevButton = -1;
    }
}

// ========================= ACCELEROMETER/I2C SECTION =========================
// Display I2C buffer contents (returns value of reading)
int DisplayBuffer(unsigned char *pucDataBuf, unsigned char ucLen) {
    unsigned char ucBufIndx = 0;
    int buffer;
    while(ucBufIndx < ucLen)
    {
        buffer = (int) pucDataBuf[ucBufIndx];
        if(buffer & 0x80) {
            buffer = buffer | 0xffffff00;
        }
        ucBufIndx++;
    }
    return buffer; // modified to return only value of reading
}

// Process I2C read register command
int ProcessReadRegCommand(char *pcInpString) {
    unsigned char ucDevAddr, ucRegOffset, ucRdLen;
    unsigned char aucRdDataBuf[256];
    char *pcErrPtr;

    // Get the device address
    pcInpString = strtok(NULL, " ");
    RETERR_IF_TRUE(pcInpString == NULL);
    ucDevAddr = (unsigned char)strtoul(pcInpString+2, &pcErrPtr, 16);

    // Get the register offset address
    pcInpString = strtok(NULL, " ");
    RETERR_IF_TRUE(pcInpString == NULL);
    ucRegOffset = (unsigned char)strtoul(pcInpString+2, &pcErrPtr, 16);

    // Get the length of data to be read
    pcInpString = strtok(NULL, " ");
    RETERR_IF_TRUE(pcInpString == NULL);
    ucRdLen = (unsigned char)strtoul(pcInpString, &pcErrPtr, 10);

    // Write the register address to be read from.
    // Stop bit implicitly assumed to be 0.
    RET_IF_ERR(I2C_IF_Write(ucDevAddr,&ucRegOffset,1,0));

    // Read the specified length of data
    RET_IF_ERR(I2C_IF_Read(ucDevAddr, &aucRdDataBuf[0], ucRdLen));

    // Display the buffer over UART on successful read
    return DisplayBuffer(aucRdDataBuf, ucRdLen); // modified to return buffer contents rather than SUCCESS/FAILURE
}

// Parse and process I2C command string
int ParseNProcessCmd(char *pcCmdBuffer) {
    char *pcInpString;
    int iRetVal = FAILURE;

    pcInpString = strtok(pcCmdBuffer, " \n\r");
    if(pcInpString != NULL)

    {

        if(!strcmp(pcInpString, "readreg"))
        {
            // Invoke the readreg command handler
            iRetVal = ProcessReadRegCommand(pcInpString);
        }
        else
        {
            UART_PRINT("Unsupported command\n\r");
            return FAILURE;
        }
    }    return iRetVal;
}

// ========================= ACCELEROMETER SHIP CONTROL SECTION =========================
// Read accelerometer value for specified axis using command-based I2C access
// axis: 'X' for X-axis (register 0x5), 'Y' for Y-axis (register 0x3)
int readAccelAxis(char axis) {
    static int error_count_x = 0;
    static int error_count_y = 0;
    char cmdBuffer[64];
    int accelValue = 0;
    int register_addr;
    int *error_count;
    const char *axis_name;

    // Determine register address and error counter based on axis
    if (axis == 'X' || axis == 'x') {
        register_addr = 0x5;
        error_count = &error_count_x;
        axis_name = "X";
    } else if (axis == 'Y' || axis == 'y') {
        register_addr = 0x3;
        error_count = &error_count_y;
        axis_name = "Y";
    } else {
        Report("Invalid axis specified: %c\r\n", axis);
        return 0;
    }

    // Create command string: "readreg 0x18 0x[register] 1" (device 0x18, register based on axis, read 1 byte)
    sprintf(cmdBuffer, "readreg 0x18 0x%x 1", register_addr);

    // Process the command using the original I2C command approach
    int result = ParseNProcessCmd(cmdBuffer);

    // Check if command succeeded
    if (result == FAILURE) {
        (*error_count)++;
        if (*error_count % 100 == 1) { // Report every 100th error to avoid spam
            Report("Command-based I2C read error for accel %s (device 0x18, reg 0x%x), error count: %d\r\n",
                   axis_name, register_addr, *error_count);
        }
        return 0; // Return neutral value on error
    }

    // Reset error count on successful read
    if (*error_count > 0) {
        *error_count = 0;
    }

    // The result is already processed and sign-extended by DisplayBuffer
    accelValue = result;

    return accelValue;
}

// Wrapper functions for backward compatibility
int readAccelX() {
    return readAccelAxis('X');
}

int readAccelY() {
    return readAccelAxis('Y');
}

// Update ship movement based on accelerometer readings
void updateShipFromAccel() {
    static int accel_counter = 0;
    static int cached_accel_x = 0;
    static int cached_accel_y = 0;
    static int debug_counter = 0;
    static int last_movement_report = 0;

    // Read accelerometer every 2 frames to reduce I2C overhead (increased from 3 for more responsiveness)
    accel_counter++;
    if (accel_counter >= 2) {
        int new_accel_x = readAccelX();
        int new_accel_y = readAccelY();

        // Only update if we got valid readings (non-zero or first read)
        if (new_accel_x != 0 || new_accel_y != 0 || (cached_accel_x == 0 && cached_accel_y == 0)) {
            cached_accel_x = new_accel_x;
            cached_accel_y = new_accel_y;
        }

        accel_counter = 0;

        // Debug output every 20 readings (more frequent for troubleshooting)
        debug_counter++;
        if (debug_counter >= 20) {
            Report("Accel raw values - X: %d, Y: %d (Device 0x18 responding: %s)\r\n",
                   cached_accel_x, cached_accel_y,
                   (cached_accel_x != 0 || cached_accel_y != 0) ? "YES" : "NO");
            debug_counter = 0;
        }
    }    // Apply accelerometer offset correction for better balance
    int x_speed_calc = cached_accel_x;
    int y_speed_calc = cached_accel_y;

    // Add deadzone to reduce jitter and improve balance
    const int deadzone = 3;  // Ignore small movements within this range
    if (x_speed_calc > -deadzone && x_speed_calc < deadzone) {
        x_speed_calc = 0;
    }

    // Apply movement with reduced scaling factor (changed from 2 to 1)
    x_speed = -1 * x_speed_calc;    // Reduced speed: Move left/right based on tilt
    y_speed = 0;                    // Disable vertical movement - ship only moves left/right

    // Clamp speeds to reasonable values for smooth gameplay (reduced limits)
    if (x_speed > 2) x_speed = 2;   // Reduced from 4 to 2
    if (x_speed < -2) x_speed = -2; // Reduced from -4 to -2
    // No Y-speed clamping needed since y_speed is always 0

    // Report movement changes and accelerometer status
    int movement_changed = (x_speed != last_movement_report);
    if (movement_changed || debug_counter == 0) {
        Report("Ship control - Speed: X=%d (Y=0-disabled) | Accel raw: X=%d, Y=%d | Calc: X=%d, Y=%d | Deadzone: %d\r\n",
               x_speed, cached_accel_x, cached_accel_y, x_speed_calc, y_speed_calc, deadzone);
        last_movement_report = x_speed;

        // Additional diagnostic information
        if (cached_accel_x == 0 && cached_accel_y == 0) {
            Report("WARNING: Accelerometer returning zero values - check I2C connection to device 0x18\r\n");
        }
    }
}

// ========================= AWS/IoT SECTION =========================
// Build JSON message for AWS IoT
void buildJson() {
    snprintf(DATA1, sizeof(DATA1),
        "{"
        "\"state\": {\r\n"
            "\"desired\" : {\r\n"
                "\"highscore\" : \"%s\"\r\n"
            "}"
        "}"
        "}\r\n\r\n", MessageTx);
}

// Set device time for secure connections
static int set_time() {
    long retVal;
    g_time.tm_day = DATE;
    g_time.tm_mon = MONTH;
    g_time.tm_year = YEAR;
    g_time.tm_sec = HOUR;
    g_time.tm_hour = MINUTE;
    g_time.tm_min = SECOND;
    retVal = sl_DevSet(SL_DEVICE_GENERAL_CONFIGURATION,
                          SL_DEVICE_GENERAL_CONFIGURATION_DATE_TIME,
                          sizeof(SlDateTime),(unsigned char *)(&g_time));
    ASSERT_ON_ERROR(retVal);
    return SUCCESS;
}

// Send message to AWS IoT via HTTP POST
void awsMsg() {
    UART_PRINT("Calling http_post with socket ID: %d\n\r", lRetVal);
    http_post(lRetVal);
    msgIndex = 0;
    memset(MessageTx, 0, sizeof(MessageTx)); // Properly clear the buffer
}

static int http_get(int iTLSSockID) {
    char acSendBuff[512];
    char acRecvbuff[1460];
    char* pcBufHeaders;
    int lRetVal = 0;

    // Compose GET request
    pcBufHeaders = acSendBuff;
    strcpy(pcBufHeaders, "GET /things/CC3200/shadow HTTP/1.1\r\n");
    pcBufHeaders += strlen("GET /things/CC3200/shadow HTTP/1.1\r\n");
    strcpy(pcBufHeaders, HOSTHEADER);
    pcBufHeaders += strlen(HOSTHEADER);
    strcpy(pcBufHeaders, CHEADER);
    pcBufHeaders += strlen(CHEADER);
    strcpy(pcBufHeaders, "\r\n");

    UART_PRINT(acSendBuff);

    // Send GET request
    lRetVal = sl_Send(iTLSSockID, acSendBuff, strlen(acSendBuff), 0);
    if (lRetVal < 0) {
        UART_PRINT("GET failed. Error Number: %i\n\r", lRetVal);
        sl_Close(iTLSSockID);
        return lRetVal;
    }

    // Receive response
    lRetVal = sl_Recv(iTLSSockID, &acRecvbuff[0], sizeof(acRecvbuff), 0);
    if (lRetVal < 0) {
        UART_PRINT("Receive failed. Error Number: %i\n\r", lRetVal);
        return lRetVal;
    } else {
        acRecvbuff[lRetVal] = '\0'; // Null-terminate the response
        UART_PRINT(acRecvbuff);
        UART_PRINT("\n\r\n\r");        // Extract highscore from JSON response
        char* highscore_ptr = strstr(acRecvbuff, "\"highscore\"");
        if (highscore_ptr) {
            char extractedHighscore[20] = {0};
            // Try multiple format patterns to handle different JSON formatting
            int parsed = 0;

            // First try format with space: "highscore" : "value"
            if (sscanf(highscore_ptr, "\"highscore\" : \"%19[^\"]\"", extractedHighscore) == 1) {
                parsed = 1;
            }
            // Then try format without space: "highscore":"value"
            else if (sscanf(highscore_ptr, "\"highscore\":\"%19[^\"]\"", extractedHighscore) == 1) {
                parsed = 1;
            }
            // Try format with varying spaces: "highscore" :"value" or "highscore": "value"
            else if (sscanf(highscore_ptr, "\"highscore\"%*[ :]%*[ ]\"%19[^\"]\"", extractedHighscore) == 1) {
                parsed = 1;
            }
              if (parsed) {
                UART_PRINT("Extracted highscore: %s\n\r", extractedHighscore);
                // Convert to int
                int highscore_value = atoi(extractedHighscore);
                UART_PRINT("Highscore as integer: %d\n\r", highscore_value);
                return highscore_value; // Return the parsed high score
            } else {
                UART_PRINT("Failed to parse highscore value from JSON\n\r");
            }
        } else {
            UART_PRINT("highscore not found in response.\n\r");
        }
    }    return -1; // Return -1 to indicate failure
}

// Get high score from AWS IoT
int getHighScoreFromAWS() {
    UART_PRINT("Retrieving high score from AWS IoT...\n\r");

    // Use the global socket ID (lRetVal) for the GET request
    int awsHighScore = http_get(lRetVal);

    if (awsHighScore >= 0) {
        UART_PRINT("Successfully retrieved AWS high score: %d\n\r", awsHighScore);
        return awsHighScore;
    } else {
        UART_PRINT("Failed to retrieve high score from AWS, using default value 0\n\r");
        return 0; // Return 0 as fallback if AWS retrieval fails
    }
}

// Perform HTTP POST to AWS IoT endpoint
static int http_post(int iTLSSockID){
    char acSendBuff[512];
    char acRecvbuff[1460];
    char cCLLength[200];
    char* pcBufHeaders;
    int lRetVal = 0;
    pcBufHeaders = acSendBuff;
    strcpy(pcBufHeaders, POSTHEADER);
    pcBufHeaders += strlen(POSTHEADER);
    strcpy(pcBufHeaders, HOSTHEADER);
    pcBufHeaders += strlen(HOSTHEADER);
    strcpy(pcBufHeaders, CHEADER);
    pcBufHeaders += strlen(CHEADER);
    strcpy(pcBufHeaders, "\r\n\r\n");
    int dataLength = strlen(DATA1);
    strcpy(pcBufHeaders, CTHEADER);
    pcBufHeaders += strlen(CTHEADER);
    strcpy(pcBufHeaders, CLHEADER1);
    pcBufHeaders += strlen(CLHEADER1);
    sprintf(cCLLength, "%d", dataLength);
    strcpy(pcBufHeaders, cCLLength);
    pcBufHeaders += strlen(cCLLength);
    strcpy(pcBufHeaders, CLHEADER2);
    pcBufHeaders += strlen(CLHEADER2);
    strcpy(pcBufHeaders, DATA1);
    pcBufHeaders += strlen(DATA1);
    int testDataLength = strlen(pcBufHeaders);
    UART_PRINT(acSendBuff);
    // Send the packet to the server
    lRetVal = sl_Send(iTLSSockID, acSendBuff, strlen(acSendBuff), 0);
    if(lRetVal < 0) {
        UART_PRINT("POST failed. Error Number: %i\n\r",lRetVal);
        sl_Close(iTLSSockID);
        return lRetVal;
    }
    lRetVal = sl_Recv(iTLSSockID, &acRecvbuff[0], sizeof(acRecvbuff), 0);
    if(lRetVal < 0) {
        UART_PRINT("Received failed. Error Number: %i\n\r",lRetVal);
        //sl_Close(iSSLSockID);
        return lRetVal;
    }
    else {
        acRecvbuff[lRetVal+1] = '\0';
        UART_PRINT(acRecvbuff);
        UART_PRINT("\n\r\n\r");
    }
    return 0;
}

// ========================= TARGETS SECTION =========================
// Draw the player ship as a simple circle
void drawShip(int x, int y, int size, unsigned int color) {
    int radius = size / 2;
    drawCircle(x, y, radius, color);
}

// Draw a simple asteroid as a square
void drawAsteroidPolygon(int cx, int cy, int radius, int sides, unsigned int color) {
    // Calculate square boundaries
    int leftmost_edge = cx - radius;
    int rightmost_edge = cx + radius;
    int topmost_edge = cy - radius;
    int bottommost_edge = cy + radius;

    // Only draw if any part of the asteroid is on or entering the screen
    if (rightmost_edge >= 0 && leftmost_edge <= SCREEN_WIDTH &&
        bottommost_edge >= 0 && topmost_edge <= SCREEN_HEIGHT) {

        // Calculate visible portion
        int visible_left = (leftmost_edge < 0) ? 0 : leftmost_edge;
        int visible_right = (rightmost_edge > SCREEN_WIDTH) ? SCREEN_WIDTH : rightmost_edge;
        int visible_top = (topmost_edge < 0) ? 0 : topmost_edge;
        int visible_bottom = (bottommost_edge > SCREEN_HEIGHT) ? SCREEN_HEIGHT : bottommost_edge;

        // Draw the square as filled rectangle using fast horizontal lines
        int y;
        for (y = visible_top; y < visible_bottom; y++) {
            if (y >= 0 && y < SCREEN_HEIGHT) {
                // Draw horizontal line for this row of the square
                int line_width = visible_right - visible_left;
                if (line_width > 0) {
                    drawFastHLine(visible_left, y, line_width, color);
                }
            }
        }
    }
}

// Initialize target positions, velocities, and colors with fixed speed tiers
void initAsteroids() {
    initAsteroidSlots();
    current_num_asteroids = 1;
    last_milestone_reached = 0;
    next_milestone = SCORE_MILESTONE_BASE;
    int i;
    for (i = 0; i < MAX_ASTEROIDS; i++) {
        asteroids[i].x = 0;
        asteroids[i].y = 0;
        asteroids[i].dx = 0;
        asteroids[i].dy = 0;
        asteroids[i].radius = 0;
        asteroids[i].sides = 0;
        asteroids[i].speed = 0;
    }
    spawnAsteroidInSlot(&asteroids[0], 0);
    setAsteroidSlotUsed(0, 1);
    Report("Dynamic asteroid system initialized with 1 asteroid: pos(%d,%d), velocity(%d,%d), radius=%d, speed=%d\r\n",
           asteroids[0].x, asteroids[0].y, asteroids[0].dx, asteroids[0].dy, asteroids[0].radius, asteroids[0].speed);
}

// ========================= DYNAMIC ASTEROID SPAWNING SYSTEM =========================
// Check if current score has reached new milestones and spawn additional asteroids
void checkScoreMilestones() {
    // Check if we've reached a new milestone
    if (player_score >= next_milestone && current_num_asteroids < MAX_ACTIVE_ASTEROIDS) {
        // Calculate which milestone level we've reached
        int milestone_level = 0;
        int temp_milestone = SCORE_MILESTONE_BASE;

        // Find the current milestone level (100, 1000, 10000, etc.)
        while (player_score >= temp_milestone) {
            milestone_level++;
            temp_milestone *= 10;
        }

        // Only spawn if this is a new milestone level
        if (milestone_level > last_milestone_reached) {
            int new_asteroids_spawned = spawnNewAsteroidSafely();
            if (new_asteroids_spawned > 0) {
                last_milestone_reached = milestone_level;
                current_num_asteroids++;
                next_milestone = temp_milestone; // Set next milestone

                Report("Score milestone reached! Score: %d, Level: %d, Active asteroids: %d, Next milestone: %d\r\n",
                       player_score, milestone_level, current_num_asteroids, next_milestone);
            }
        }
    }
}


// Check if a position is safe for spawning (no overlaps with existing asteroids)
int isPositionSafe(int x, int y, int radius) {
    int i;
    int min_safe_distance = 50; // Minimum distance between asteroid centers

    for (i = 0; i < current_num_asteroids; i++) {
        // Skip uninitialized asteroids
        if (asteroids[i].radius == 0) continue;

        // Calculate distance between centers
        int dx = x - asteroids[i].x;
        int dy = y - asteroids[i].y;
        int distance_squared = dx * dx + dy * dy;
        int required_distance = radius + asteroids[i].radius + min_safe_distance;

        // Check if too close (overlapping or too near)
        if (distance_squared < required_distance * required_distance) {
            return 0; // Not safe
        }
    }

    return 1; // Position is safe
}

void MasterMain() {
    // Reset SPI
    MAP_SPIReset(GSPI_BASE);

    // Configure SPI interface
    MAP_SPIConfigSetExpClk(GSPI_BASE, MAP_PRCMPeripheralClockGet(PRCM_GSPI),
        SPI_IF_BIT_RATE, SPI_MODE_MASTER, SPI_SUB_MODE_0,
        (SPI_SW_CTRL_CS |
         SPI_4PIN_MODE |
         SPI_TURBO_OFF |
         SPI_CS_ACTIVEHIGH |
         SPI_WL_8));

    // Enable SPI for communication
    MAP_SPIEnable(GSPI_BASE);
}

// Print a string on the OLED using drawChar for each character
void printOLED(const char msg[], int x, int y, unsigned int color) {
    int c;
    const int charWidth = 6; // Character width
    for (c = 0; c < strlen(msg); c++) {
        drawChar(x + (c * charWidth), y, msg[c], color, BLACK, 1);
    }
}

void i2cInit() {
    I2C_IF_Open(I2C_MASTER_MODE_FST);
    Report("I2C interface opened\r\n");

    // Initialize accelerometer with enhanced configuration
    unsigned char ucDevAddr = 0x18;  // Updated accelerometer device address

    // First, put device in standby mode before configuring
    unsigned char ucRegOffset = 0x2A; // CTRL_REG1 - Control register
    unsigned char ucStandbyData = 0x00; // Standby mode (bit 0 = 0)

    Report("Setting accelerometer to standby mode...\r\n");
    int writeResult = I2C_IF_Write(ucDevAddr, &ucRegOffset, 1, 0);
    if (writeResult == SUCCESS) {
        writeResult = I2C_IF_Write(ucDevAddr, &ucStandbyData, 1, 1);
        if (writeResult != SUCCESS) {
            Report("Failed to set accelerometer to standby mode\r\n");
            return;
        }
    } else {
        Report("Failed to access accelerometer CTRL_REG1\r\n");
        return;
    }

    // Configure data rate and range
    ucRegOffset = 0x2A; // CTRL_REG1
    unsigned char ucActiveData = 0x19; // Active mode (bit 0=1) + 50Hz ODR (bits 5:3=011)

    Report("Configuring accelerometer for active mode with 50Hz data rate...\r\n");
    writeResult = I2C_IF_Write(ucDevAddr, &ucRegOffset, 1, 0);
    if (writeResult == SUCCESS) {
        writeResult = I2C_IF_Write(ucDevAddr, &ucActiveData, 1, 1);
        if (writeResult == SUCCESS) {
            Report("Accelerometer configured successfully\r\n");

            // Test read to verify functionality
            MAP_UtilsDelay(1000000); // 370ms delay for stabilization
            int testX = readAccelX();
            int testY = readAccelY();
            Report("Initial accelerometer test read - X: %d, Y: %d\r\n", testX, testY);

        } else {
            Report("Failed to activate accelerometer\r\n");
        }
    } else {
        Report("Failed to write accelerometer configuration\r\n");
    }
}

void interruptInit() {
    unsigned long ulStatus;
    MAP_GPIOIntRegister(IR_SIGNAL.port, GPIOA0IntHandler);
    MAP_GPIOIntTypeSet(IR_SIGNAL.port, IR_SIGNAL.pin, GPIO_RISING_EDGE);
    ulStatus = MAP_GPIOIntStatus(IR_SIGNAL.port, false);
    MAP_GPIOIntClear(IR_SIGNAL.port, ulStatus);
    IR_intcount=0; IR_intflag=0;
    MAP_GPIOIntEnable(IR_SIGNAL.port, IR_SIGNAL.pin);
    g_ulBase = TIMERA0_BASE;
    Timer_IF_Init(PRCM_TIMERA0, g_ulBase, TIMER_CFG_PERIODIC, TIMER_A, 0);
    Timer_IF_IntSetup(g_ulBase, TIMER_A, TimerBaseIntHandler);
}


