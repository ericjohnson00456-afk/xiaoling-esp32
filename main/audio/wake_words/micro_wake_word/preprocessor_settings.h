#pragma once

// Audio settings
#define AUDIO_SAMPLE_FREQUENCY 16000
#define FEATURE_DURATION_MS 30
#define PREPROCESSOR_FEATURE_SIZE 40

// Frontend configuration constants
#define FRONTEND_WINDOW_SIZE_MS 30
#define FRONTEND_WINDOW_STEP_SIZE_MS 10
#define FRONTEND_FILTERBANK_NUM_CHANNELS 40
#define FRONTEND_LOWER_BAND_LIMIT 125.0f
#define FRONTEND_UPPER_BAND_LIMIT 7500.0f

// Buffer sizes
#define SAMPLE_RATE_HZ 16000
#define BUFFER_LENGTH 64  // 0.064 seconds
#define BUFFER_SIZE (SAMPLE_RATE_HZ / 1000 * BUFFER_LENGTH)
#define INPUT_BUFFER_SIZE (16 * SAMPLE_RATE_HZ / 1000)  // 16ms * 16kHz / 1000ms

// The number of audio slices to process before accepting a positive detection
#define MIN_SLICES_BEFORE_DETECTION 74
