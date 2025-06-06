#include "vimDAW.hpp"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <map>
#include <math.h>
#include <ranges>
#include <stdlib.h>
#include <string>
#include <vector>

const int COLS = 66;            // small: 34 big: 66
const int ROWS = 40;            // small: 20 big: 40
const int NOTE_COLS = COLS - 2; // -2
const int NOTE_ROWS = ROWS - 3; // -3

// cursor blinking
const Uint64 BLINK_INTERVAL = 500;
const Uint64 MOVE_RESET_TIME = 500;

// rendering piano roll buffers
const float BUFFER_WIDTH = 16;
const float BUFFER_HEIGHT = 127;
const float BUFFER_SIZE = BUFFER_WIDTH * BUFFER_HEIGHT;

// visible part of piano roll
const float VIEWPORT_WIDTH = 20;
const float VIEWPORT_HEIGHT = 20;
const float VIEWPORT_SIZE = VIEWPORT_HEIGHT * VIEWPORT_WIDTH;

// playback bar
const float PIXELS_PER_SECOND{100.0f};

/*
 * buffer map
 *
 * lookahead scheduling (process only milliseconds ahead)
 * for realtime updates (when they add note after playback it actually plays it)
 */
std::map<int, std::vector<Note>> buffer_map;

// default resolution
float WINDOW_WIDTH;
float WINDOW_HEIGHT;

// note pixel dimensions
float NOTE_WIDTH;
float NOTE_HEIGHT;

SDL_Window *gSDLWindow;
SDL_Renderer *gSDLRenderer;
TTF_Font *font;
int gDone;

std::vector<Note> notes;
std::vector<SDL_FRect> dirty_rects;

float x, y;
int cursor_col = 2;
int cursor_row = 0;

// blinking cursor
bool cursor_visible = true;
Uint64 lastToggleTime = 0;
Uint64 lastMovedTime = 0;

// modes
bool normal_mode = true;
bool visual_mode = false;
int visual_col = 2;
int visual_row = 0;

// threading
extern std::atomic<bool> play_audio;
extern std::condition_variable play_audio_cv;
std::atomic<bool> program_on{true};
std::mutex notes_mtx;
extern std::atomic<double> gui_playback_time;

bool contains_note(int start, int note) {
    return std::any_of(notes.begin(), notes.end(),
                       [start, note](const Note &entry) {
                           return entry.start == start && entry.note == note;
                       });
}

void add_note(int start, int end, int note) {
    Note entry = {start, end, note};
    if (contains_note(start, note)) {
        return;
    }
    notes.push_back(entry);
}

Note *get_note(int start, int note) {
    auto it = std::find_if(
        notes.begin(), notes.end(), [start, note](const Note &entry) {
            return entry.start == start && entry.note == note;
        });

    return (it != notes.end()) ? &(*it) : nullptr;
}

void remove_note(int start, int note) {
    notes.erase(std::remove_if(notes.begin(), notes.end(),
                               [start, note](const Note &entry) {
                                   return entry.start == start &&
                                          entry.note == note;
                               }),
                notes.end());
}

void mark_dirty(float x, float y, float w, float h) {
    SDL_FRect dirtyRect = {x, y, w, h};
    dirty_rects.push_back(dirtyRect);
}

bool contains_rect(float x, float y) {
    return std::any_of(
        dirty_rects.begin(), dirty_rects.end(),
        [x, y](const SDL_FRect &rect) { return rect.x == x && rect.y == y; });
}

void remove_rect(float x, float y) {
    dirty_rects.erase(std::remove_if(dirty_rects.begin(), dirty_rects.end(),
                                     [x, y](const SDL_FRect &rect) {
                                         return rect.x == x && rect.y == y;
                                     }),
                      dirty_rects.end());
}

int note_col(int col) { return col - 2; }

int note_row(int row) { return (NOTE_ROWS - row) + 43; }

int render_col(int col) { return col + 2; }

int render_row(int row) { return NOTE_ROWS - row; }

void update_notes() {
    std::lock_guard<std::mutex> lock(notes_mtx);

    // remove note
    if (contains_note(note_col(cursor_col), note_row(cursor_row))) {
        x = static_cast<float>(cursor_col) * WINDOW_WIDTH / COLS + 1;
        y = static_cast<float>(cursor_row) * WINDOW_HEIGHT / ROWS + 1;
        remove_note(note_col(cursor_col), note_row(cursor_row));
        remove_rect(x, y);

        return;
    }

    // check if long note
    bool contains = false;
    int col;
    for (int i = 1; i <= cursor_col && !contains; i++) {
        Note *note = get_note(note_col(cursor_col - i), note_row(cursor_row));
        if (!note) {
            continue;
        }
        if (render_col(note->start) + note->duration() > cursor_col) {
            contains = true;
            col = render_col(note->start);
        }
    }

    // remove long note
    if (contains) {
        x = static_cast<float>(col) * WINDOW_WIDTH / COLS + 1;
        y = static_cast<float>(cursor_row) * WINDOW_HEIGHT / ROWS + 1;
        remove_note(note_col(col), note_row(cursor_row));
        remove_rect(x, y);

        return;
    }

    // draw note
    if (!contains && normal_mode) {
        x = static_cast<float>(cursor_col) * WINDOW_WIDTH / COLS + 1;
        y = static_cast<float>(cursor_row) * WINDOW_HEIGHT / ROWS + 1;
        add_note(note_col(cursor_col), note_col(cursor_col + 1),
                 note_row(cursor_row));
        mark_dirty(x, y, NOTE_WIDTH - 2, NOTE_HEIGHT - 2);

        return;
    }

    // draw long note
    if (!contains && visual_mode) {
        bool from_left = cursor_col > visual_col;
        int duration = std::abs(cursor_col - visual_col);
        int start = cursor_col;
        if (from_left) {
            start = cursor_col - duration;
        }
        // check for overlap
        for (int i = 0; i < duration; i++) {
            Note *note = get_note(note_col(start + i), note_row(cursor_row));
            if (note) {
                return;
            }
        }
        x = static_cast<float>(start) * WINDOW_WIDTH / COLS + 1;
        y = static_cast<float>(cursor_row) * WINDOW_HEIGHT / ROWS + 1;
        add_note(note_col(start), note_col(start) + duration + 1,
                 note_row(cursor_row));
        mark_dirty(x, y, NOTE_WIDTH * (duration + 1) - 2, NOTE_HEIGHT - 2);

        return;
    }
}

void handle_input(SDL_Event e, bool *moved) {
    const bool *key_states = SDL_GetKeyboardState(NULL);
    if (key_states[SDL_SCANCODE_LCTRL] || key_states[SDL_SCANCODE_RCTRL]) {
        if (key_states[SDL_SCANCODE_J] || key_states[SDL_SCANCODE_D]) {
            *moved = true;
            cursor_row += 3;
            if (cursor_row >= ROWS - 3) {
                cursor_row = ROWS - 4;
            }
        }
        if (key_states[SDL_SCANCODE_K] || key_states[SDL_SCANCODE_U]) {
            *moved = true;
            cursor_row -= 3;
            if (cursor_row < 0) {
                cursor_row = 0;
            }
        }
        if (key_states[SDL_SCANCODE_L]) {
            *moved = true;
            cursor_col += 3;
            if (cursor_col >= COLS - 1) {
                cursor_col = COLS - 1;
            }
        }
        if (key_states[SDL_SCANCODE_H]) {
            *moved = true;
            cursor_col -= 3;
            if (cursor_col < 2) {
                cursor_col = 2;
            }
        }
    }
    if (key_states[SDL_SCANCODE_LSHIFT] || key_states[SDL_SCANCODE_RSHIFT]) {
        if (key_states[SDL_SCANCODE_4]) {
            *moved = true;
            cursor_col = COLS - 1;
        }
        if (key_states[SDL_SCANCODE_G]) {
            *moved = true;
            cursor_row = ROWS - 4;
        }
    }
    switch (e.type) {
    case SDL_EVENT_QUIT:
        gDone = 1;
        break;
    case SDL_EVENT_KEY_DOWN:
        switch (e.key.key) {
        case SDLK_ESCAPE:
            if (visual_mode) {
                visual_mode = false;
                normal_mode = true;
                break;
            }
            gDone = 1;
            program_on.store(false);
            play_audio.store(false);
            play_audio_cv.notify_one();
            play_audio.notify_one();
            break;
        case SDLK_SPACE:
            play_audio.store(!play_audio.load());
            play_audio_cv.notify_one();
            break;
        case SDLK_H:
            *moved = true;
            cursor_col--;
            if (cursor_col < 2) {
                cursor_col = 2;
            }
            break;
        case SDLK_J:
            *moved = true;
            cursor_row++;
            if (cursor_row >= ROWS - 3) {
                cursor_row = ROWS - 4;
            }
            break;
        case SDLK_K:
            *moved = true;
            cursor_row--;
            if (cursor_row < 0) {
                cursor_row = 0;
            }
            break;
        case SDLK_L:
            *moved = true;
            cursor_col++;
            if (cursor_col >= COLS) {
                cursor_col = COLS - 1;
            }
            break;
        case SDLK_X:
        case SDLK_RETURN:
            *moved = true;
            update_notes();
            if (visual_mode) {
                visual_mode = false;
                normal_mode = true;
            }
            break;
        case SDLK_0:
            *moved = true;
            cursor_col = 2;
            break;
        case SDLK_G:
            if (!key_states[SDL_SCANCODE_LSHIFT] &&
                !key_states[SDL_SCANCODE_RSHIFT]) {
                *moved = true;
                cursor_row = 0;
            }
            break;
        case SDLK_V:
            *moved = true;
            if (normal_mode) {
                visual_mode = true;
                normal_mode = false;
                visual_col = cursor_col;
                visual_row = cursor_row;
                break;
            }
            if (visual_mode) {
                normal_mode = true;
                visual_mode = false;
            }
            break;
        default:
            break;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        switch (e.button.button) {
        case SDL_BUTTON_LEFT:
            // attempt (not working I think):
            // SDL_GetMouseState(&x, &y);
            // x -= std::fmod(x, NOTE_WIDTH);
            // y -= std::fmod(y, NOTE_HEIGHT);
            // update_notes();
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

void render_playback_bar() {
    double time = gui_playback_time.load(std::memory_order_relaxed);
    float x = static_cast<int>(time * PIXELS_PER_SECOND);

    SDL_SetRenderDrawColor(gSDLRenderer, 0xFF, 0x00, 0x00, 0xFF);
    SDL_RenderLine(gSDLRenderer, x, 0, x, WINDOW_HEIGHT - NOTE_HEIGHT * 3);
}

void initial_render() {
    // Draw piano
    SDL_SetRenderDrawColor(gSDLRenderer, 0xFF, 0xFF, 0xFF, 0xFF);
    SDL_FRect piano;
    piano = {0, 0, NOTE_WIDTH * 2 - 1, NOTE_HEIGHT * (ROWS - 3) - 1};
    SDL_RenderFillRect(gSDLRenderer, &piano);

    SDL_Color text_color = {0x00, 0x00, 0x00};

    // horizontal lines
    std::string notes[12] = {"G#X", "GX",  "F#X", "FX", "EX",  "D#X",
                             "DX",  "C#X", "CX",  "BX", "A#X", "AX"};
    int index = 0;
    for (int row = 0; row <= ROWS - 3; row++) {
        int y_pos = row * NOTE_HEIGHT;
        SDL_SetRenderDrawColor(gSDLRenderer, 0x00, 0x00, 0x00, 0xFF);
        SDL_RenderLine(gSDLRenderer, 0, y_pos, WINDOW_WIDTH, y_pos);
        if (row % 12 == 0) {
            index = 0;
        }
        if (notes[index][1] == '#') {
            SDL_FRect black;
            piano = {0, (float)y_pos, (NOTE_WIDTH * 2 * 3 / 4) - 1,
                     NOTE_HEIGHT};
            SDL_RenderFillRect(gSDLRenderer, &piano);
        } else if (notes[index][1] != '#') {
            SDL_Surface *text_surface = TTF_RenderText_Blended(
                font, notes[index].c_str(), 2, text_color);
            if (!text_surface) {
                SDL_Log("TTF_RenderText_Blended Error: %s", SDL_GetError());
                continue;
            }
            SDL_Texture *text_texture =
                SDL_CreateTextureFromSurface(gSDLRenderer, text_surface);
            if (!text_texture) {
                SDL_Log("SDL_CreateTextureFromSurface Error: %s",
                        SDL_GetError());
                continue;
            }

            SDL_FRect text = {NOTE_WIDTH * 3 / 4, (float)y_pos,
                              (float)text_surface->w, (float)text_surface->h};
            SDL_RenderTexture(gSDLRenderer, text_texture, NULL, &text);
            SDL_DestroySurface(text_surface);
            SDL_DestroyTexture(text_texture);
        }
        index++;
    }

    // vertical lines
    for (int col = 2; col <= COLS; col++) {
        int x_pos = col * NOTE_WIDTH;
        if ((col - 2) % 4 == 0 && (col - 2) > 0) {
            SDL_SetRenderDrawColor(gSDLRenderer, 0x54, 0x54, 0x54, 0xFF);
            SDL_RenderLine(gSDLRenderer, x_pos, 0, x_pos,
                           WINDOW_HEIGHT - NOTE_HEIGHT * 3);
            SDL_SetRenderDrawColor(gSDLRenderer, 0, 0, 0, 0xFF);
            continue;
        }
        SDL_RenderLine(gSDLRenderer, x_pos, 0, x_pos,
                       WINDOW_HEIGHT - NOTE_HEIGHT * 3);
    }

    // bar1
    SDL_SetRenderDrawColor(gSDLRenderer, 0x2A, 0x27, 0x3F, 0xFF);
    SDL_FRect status_bar;
    status_bar = {0, float((ROWS - 3) * NOTE_HEIGHT), NOTE_WIDTH * COLS,
                  NOTE_HEIGHT};
    SDL_RenderFillRect(gSDLRenderer, &status_bar);

    // text1
    std::string window_name = "roll1";
    std::string spaces;
    // have to cast 2nd arg in ::iota to signed integer (need both args to be
    // unsigned or both as signed)
    for (int i :
         std::views::iota(0, COLS - static_cast<int>(window_name.length()))) {
        // need to adjust relative to zoom
        spaces.push_back(' ');
    }
    text_color = {0x6E, 0x6C, 0x7E};
    SDL_Surface *text_surface = TTF_RenderText_Blended(
        font,
        (window_name + spaces + std::to_string(cursor_row + 1) + "," +
         std::to_string(cursor_col - 1))
            .c_str(),
        0, text_color);
    if (!text_surface) {
        SDL_Log("TTF_RenderText_Blended Error: %s", SDL_GetError());
    }
    SDL_Texture *text_texture =
        SDL_CreateTextureFromSurface(gSDLRenderer, text_surface);
    if (!text_texture) {
        SDL_Log("SDL_CreateTextureFromSurface Error: %s", SDL_GetError());
    }
    SDL_FRect text = {0, float((ROWS - 3) * NOTE_HEIGHT),
                      (float)text_surface->w, (float)text_surface->h};
    SDL_RenderTexture(gSDLRenderer, text_texture, NULL, &text);
    SDL_DestroySurface(text_surface);
    SDL_DestroyTexture(text_texture);

    // bar2
    SDL_SetRenderDrawColor(gSDLRenderer, 0x23, 0x21, 0x36, 0xFF);
    SDL_FRect command_bar;
    command_bar = {0, float((ROWS - 2) * NOTE_HEIGHT), NOTE_WIDTH * COLS,
                   NOTE_HEIGHT};
    SDL_RenderFillRect(gSDLRenderer, &command_bar);

    // text2
    text_color = {0xFF, 0xFF, 0xFF};
    std::string mode = " ";
    if (visual_mode) {
        mode = "-- VISUAL --";
    }
    SDL_Surface *text_surface2 =
        TTF_RenderText_Blended(font, mode.c_str(), 0, text_color);
    if (!text_surface2) {
        SDL_Log("TTF_RenderText_Blended Error: %s", SDL_GetError());
    }
    SDL_Texture *text_texture2 =
        SDL_CreateTextureFromSurface(gSDLRenderer, text_surface2);
    if (!text_texture2) {
        SDL_Log("SDL_CreateTextureFromSurface Error: %s", SDL_GetError());
    }
    SDL_FRect text2 = {0, float((ROWS - 2) * NOTE_HEIGHT),
                       (float)text_surface2->w, (float)text_surface2->h};
    SDL_RenderTexture(gSDLRenderer, text_texture2, NULL, &text2);
    SDL_DestroySurface(text_surface2);
    SDL_DestroyTexture(text_texture2);

    // bar3
    SDL_SetRenderDrawColor(gSDLRenderer, 0x33, 0xAF, 0xF4, 0xFF);
    SDL_FRect tmux_bar;
    tmux_bar = {0, float((ROWS - 1) * NOTE_HEIGHT), NOTE_WIDTH * COLS,
                NOTE_HEIGHT};
    SDL_RenderFillRect(gSDLRenderer, &tmux_bar);

    // text3
    text_color = {0x00, 0x00, 0x00};
    SDL_Surface *text_surface3 =
        TTF_RenderText_Blended(font, "[project1] 1:roll1*", 0, text_color);
    if (!text_surface3) {
        SDL_Log("TTF_RenderText_Blended Error: %s", SDL_GetError());
    }
    SDL_Texture *text_texture3 =
        SDL_CreateTextureFromSurface(gSDLRenderer, text_surface3);
    if (!text_texture3) {
        SDL_Log("SDL_CreateTextureFromSurface Error: %s", SDL_GetError());
    }
    SDL_FRect text3 = {0, float((ROWS - 1) * NOTE_HEIGHT),
                       (float)text_surface3->w, (float)text_surface3->h};
    SDL_RenderTexture(gSDLRenderer, text_texture3, NULL, &text3);
    SDL_DestroySurface(text_surface3);
    SDL_DestroyTexture(text_texture3);
}

bool update() {
    SDL_Event e;
    bool moved = false;
    if (visual_mode) {
        moved = true;
    }
    int number = 1;
    while (SDL_PollEvent(&e)) {
        handle_input(e, &moved);
    }

    if (moved) {
        cursor_visible = true;
        lastMovedTime = SDL_GetTicks();
    }

    // Set background color
    SDL_SetRenderDrawColor(gSDLRenderer, 0x23, 0x21, 0x36, 0xFF);
    SDL_RenderClear(gSDLRenderer);

    // Render the static grid
    initial_render();

    // Update dirty rectangles
    for (const SDL_FRect &rect : dirty_rects) {
        SDL_SetRenderDrawColor(gSDLRenderer, 0x33, 0xAF, 0xF4, 0xFF);
        SDL_RenderFillRect(gSDLRenderer, &rect);
    }

    // Render cursor as a transparent, blinking block
    Uint64 time = SDL_GetTicks();
    if (time - lastMovedTime >= MOVE_RESET_TIME) {
        if (time - lastToggleTime >= BLINK_INTERVAL) {
            cursor_visible = !cursor_visible;
            lastToggleTime = time;
        }
    } else {
        cursor_visible = true;
    }

    if (cursor_visible) {
        SDL_FRect cursorRect;
        SDL_SetRenderDrawBlendMode(gSDLRenderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gSDLRenderer, 0x39, 0x35, 0x52,
                               0x80); // gray cursor 50% opacity
        cursorRect = {float(cursor_col * NOTE_WIDTH),
                      float(cursor_row * NOTE_HEIGHT), NOTE_WIDTH, NOTE_HEIGHT};
        if (visual_mode) {
            int left_col;
            int top_row;
            int notes_width;
            int notes_height;

            int right_col;
            int bottom_row;

            int col1 = cursor_col;
            int row1 = cursor_row;

            int col2 = visual_col;
            int row2 = visual_row;

            top_row = row1;
            bottom_row = row2;
            if (row2 < row1) {
                top_row = row2;
                bottom_row = row1;
            }

            left_col = col1;
            right_col = col2;
            if (col2 < col1) {
                left_col = col2;
                right_col = col1;
            }

            notes_width = (right_col - left_col + 1) * NOTE_WIDTH;
            notes_height = (bottom_row - top_row + 1) * NOTE_HEIGHT;

            left_col *= NOTE_WIDTH;
            top_row *= NOTE_HEIGHT;
            cursorRect = {float(left_col), float(top_row), float(notes_width),
                          float(notes_height)};
        }

        SDL_RenderFillRect(gSDLRenderer, &cursorRect);
        SDL_SetRenderDrawBlendMode(gSDLRenderer, SDL_BLENDMODE_NONE);
    }

    SDL_RenderPresent(gSDLRenderer);

    return true;
}

void loop() {
    if (!update()) {
        gDone = 1;
    }
}

void render_and_input() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
        return;
    }

    // Set display resolution
    int num_displays;
    SDL_DisplayID* displays = SDL_GetDisplays(&num_displays);
    if (displays == nullptr) {
        std::cerr << "Failed to get displays: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return;
    }
    int count = 0;
    int current_avg;
    for (int i = 0; count < num_displays; ++i) {
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(i);
        if (dm == NULL) {
            continue; // Skip to the next display if there is an error
        }
        count++;
        //std::cout << "Display " << i << " Resolution: " << dm->w << "x" << dm->h
        //          << " @ " << dm->refresh_rate << "Hz" << std::endl;
        if (count == 1) {
            float width_distance = abs(1920 - dm->w);
            float height_distance = abs(1080 - dm->h);
            current_avg = (width_distance + height_distance) / 2;
            WINDOW_HEIGHT = dm->h;
            WINDOW_WIDTH = dm->w;
            continue;
        }
        float width_distance = abs(1920 - dm->w);
        float height_distance = abs(1080 - dm->h);
        float avg = (width_distance + height_distance) / 2;
        if (avg <= current_avg) {
            WINDOW_HEIGHT = dm->h;
            WINDOW_WIDTH = dm->w;
        }
    }

    // Set note dimensions
    NOTE_WIDTH = WINDOW_WIDTH / COLS;
    NOTE_HEIGHT = WINDOW_HEIGHT / ROWS;

    if (!TTF_Init()) {
        SDL_Log("SDL_ttf Error: %s", SDL_GetError());
        SDL_Quit();
        return;
    }

    font =
        TTF_OpenFont("/home/monaco/Projects/vimDAW/assets/fonts/NotoMono.ttf",
                     WINDOW_WIDTH / (COLS + 6));
    if (!font) {
        SDL_Log("TTF_OpenFont Error: %s", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
    }

    gSDLWindow =
        SDL_CreateWindow("SDL3 window", WINDOW_WIDTH, WINDOW_HEIGHT, 0);

    gSDLRenderer = SDL_CreateRenderer(gSDLWindow, NULL);

    if (!gSDLWindow || !gSDLRenderer) {
        std::cerr << "SDL_Create* Error: " << SDL_GetError() << std::endl;
        return;
    }

    gDone = 0;

    while (!gDone) {
        loop();
    }

    SDL_DestroyRenderer(gSDLRenderer);
    SDL_DestroyWindow(gSDLWindow);
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();

    return;
}
