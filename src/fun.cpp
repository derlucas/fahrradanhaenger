#include <Arduino.h>
#include <NeoPixelBus.h>
#include <NeoPixelAnimator.h>
#include "RunningMedian.h"

#define SPEED_ARRAY_SIZE    5
#define PULSES_ARRAY_SIZE   10
#define LONGEST_PULSETIME   10000

#define STRIP_A_COUNT   33          // left + right
#define STRIP_B_COUNT   30          // bottom + top
#define STRIP_C_OFFSET  1

#define TAIL_SIZE       10
const int wheel_circumference = 16 * (25.4 * PI);    // given in inches, calculated to mm
const int strip_a_length = 55; // cm

const uint16_t PixelCount = 2 * STRIP_A_COUNT + 2*STRIP_B_COUNT + STRIP_C_OFFSET;
const uint16_t PixelPin = 17;

uint32_t last_interrupt_time = 0;
int speed_index = 0;
float speed_array[SPEED_ARRAY_SIZE];
float speed = 0.0f; // in km/h
float speed_last= 0.0f; // in km/h
uint32_t pulse_time = 0;
uint32_t speed_millis = 0;
uint32_t animation_change_millis = 0;
uint8_t pulse_index = 0;

RunningMedian pulses = RunningMedian(PULSES_ARRAY_SIZE);
//uint32_t pulses[PULSES_ARRAY_SIZE];
uint32_t pulses_avg;

NeoPixelBus<NeoGrbFeature, NeoSk6812Method> strip(PixelCount, PixelPin);

typedef enum Animation {
    DRIVING,
    DRIVING_KEEP,
    DRIVING_WITH_COLORCHANGE,
    DRIVING_WITH_COLORCHANGE_KEEP,
    GLOBALCOLORFADE,
    RAINBOW_CIRCLE,
};

NeoPixelAnimator animations(1);
HsbColor drivingAnimationColor;
uint8_t drivingAnimationColorIndex = 0;
Animation currentAnimation = DRIVING;
Animation lastAnimation = DRIVING;
uint32_t animation_duration = 10;   // seconds

bool is_driving_animation() {
    return currentAnimation == DRIVING_WITH_COLORCHANGE || currentAnimation == DRIVING_WITH_COLORCHANGE_KEEP ||
        currentAnimation == DRIVING || currentAnimation == DRIVING_KEEP;
}

void set_color_all_pixels(HsbColor color) {
    uint16_t pixel_count = STRIP_A_COUNT*2 + STRIP_B_COUNT*2 + STRIP_C_OFFSET;
    for(uint16_t i = 0; i < pixel_count; i++) {
        strip.SetPixelColor(i, color);
    }
}

void anim_update_global_colorchange(const AnimationParam& param) {
    float progress = param.progress;

    HsbColor color = HsbColor(progress, 1.0f, 1.0f);
    uint16_t pixel_count = STRIP_A_COUNT*2 + STRIP_B_COUNT*2 + STRIP_C_OFFSET;

    for(uint16_t i = 0; i < pixel_count; i++) {
        strip.SetPixelColor(i, color);
    }

    if (param.state == AnimationState_Completed) {
        if(currentAnimation == GLOBALCOLORFADE) {
            animations.RestartAnimation(param.index);
        }
    }
}

void anim_update_rainbow_circle(const AnimationParam& param) {

    if (param.state == AnimationState_Completed) {
        strip.RotateRight(1);
        animations.RestartAnimation(param.index);
    }
}

void anim_update_driving(const AnimationParam& param) {
    float progress = param.progress;

    uint16_t nextPixel = progress * (STRIP_A_COUNT+TAIL_SIZE);

    for(uint16_t i = 0; i < TAIL_SIZE; i++) {
        int16_t prevPixel = nextPixel-i;
        if(prevPixel >=0 && prevPixel <= STRIP_A_COUNT) {

            float fade = (1.0f/TAIL_SIZE) * (float)(i);

            HsbColor darkerColor = HsbColor(drivingAnimationColor.H,
                                            drivingAnimationColor.S,
                                            drivingAnimationColor.B - fade );

            if((currentAnimation == DRIVING || currentAnimation == DRIVING_WITH_COLORCHANGE) && i == TAIL_SIZE-1) {
                darkerColor = HsbColor(0);
            }

            strip.SetPixelColor(prevPixel, darkerColor);
            strip.SetPixelColor(STRIP_A_COUNT+STRIP_B_COUNT+STRIP_A_COUNT-prevPixel, darkerColor);
        }
    }

    if (param.state == AnimationState_Completed) {
        if(currentAnimation == DRIVING_WITH_COLORCHANGE || currentAnimation == DRIVING_WITH_COLORCHANGE_KEEP) {
            drivingAnimationColorIndex++;
            drivingAnimationColorIndex %= 50;
            drivingAnimationColor = HsbColor(drivingAnimationColorIndex / 50.0f, 1.0f, 1.0f);
            animations.RestartAnimation(param.index);
        } else if(currentAnimation == DRIVING || currentAnimation == DRIVING_KEEP) {
            animations.RestartAnimation(param.index);
        }
    }
}

void anim_update_driving2(const AnimationParam& param) {
    float progress = param.progress;

    uint16_t nextPixel = progress * (STRIP_A_COUNT+1);


    if(nextPixel <= STRIP_A_COUNT) {
        strip.SetPixelColor(nextPixel, drivingAnimationColor);
        strip.SetPixelColor(STRIP_A_COUNT + STRIP_B_COUNT + STRIP_A_COUNT - nextPixel, drivingAnimationColor);
    }

    // set previous pixel
    if(nextPixel > 0) {
        float b = (currentAnimation == DRIVING || currentAnimation == DRIVING_WITH_COLORCHANGE) ? 0.0f : 0.02f;
        HsbColor darkerColor = HsbColor(drivingAnimationColor.H,
                                        drivingAnimationColor.S,
                                        b);

        strip.SetPixelColor(nextPixel-1, darkerColor);
        strip.SetPixelColor(STRIP_A_COUNT+STRIP_B_COUNT+STRIP_A_COUNT-nextPixel+1, darkerColor);
    }

    if (param.state == AnimationState_Completed) {
        if(currentAnimation == DRIVING_WITH_COLORCHANGE || currentAnimation == DRIVING_WITH_COLORCHANGE_KEEP) {
            drivingAnimationColorIndex++;
            drivingAnimationColorIndex %= 50;
            drivingAnimationColor = HsbColor(drivingAnimationColorIndex / 50.0f, 1.0f, 1.0f);
            animations.RestartAnimation(param.index);
        } else if(currentAnimation == DRIVING || currentAnimation == DRIVING_KEEP) {
            animations.RestartAnimation(param.index);
        }
    }
}

void wheel_sensor_interrupt() {
    uint32_t interrupt_time = millis();

    if (interrupt_time - last_interrupt_time > 40) {    // debounce
        pulse_time = interrupt_time - last_interrupt_time;
    }
    last_interrupt_time = interrupt_time;
}


void speedcalc() {
    uint32_t current_millis = millis();
    if(current_millis - last_interrupt_time > LONGEST_PULSETIME) {
        // no more pulses, reset our array
        pulses.clear();
        pulses_avg = 0;
    } else {
        // capture the pulse time
        pulses.add(pulse_time);
        pulses_avg = pulses.getMedian();
    }

//    Serial.printf("pulse_avg = %d\n", pulses_avg);

    if((pulses_avg > 0 && pulses_avg < LONGEST_PULSETIME) ) {
        uint32_t mm_per_minute = (1000 * wheel_circumference / pulses_avg) * 60;
        speed = mm_per_minute / 16667.0f;
    } else {
        speed = 0;
    }

//    Serial.printf("speed = %f\n", speed);
}



uint16_t getAnimationSpeed() {
    if(speed < 0.01f) return 10000;

    float speed_cm_per_second = speed * 27.778;
    uint16_t duration = (strip_a_length / speed_cm_per_second) * 1000;

    if(duration < 400) {
        duration = 400;
    } else if(duration > 10000) {
        duration = 10000;
    }
    return duration;
}

void start_anim_rainbow_circle() {
    animations.StopAll();

    //set_color_all_pixels(HsbColor(0));
    uint16_t pixel_count = STRIP_A_COUNT*2 + STRIP_B_COUNT*2 + STRIP_C_OFFSET;

    for (uint16_t i = 0; i < pixel_count; i++) {
        float hue = (float)i / (float)pixel_count;
        HsbColor color = HsbColor(hue, 1.0f, 1.0f);
        strip.SetPixelColor(i, color);
    }

    currentAnimation = RAINBOW_CIRCLE;
    animations.StartAnimation(0, 100, anim_update_rainbow_circle);
}

void start_anim_driving(Animation anim = DRIVING) {
    animations.StopAll();
    set_color_all_pixels(HsbColor(0));
    currentAnimation = anim;
    drivingAnimationColor = HsbColor(0.0f,1.0f,1.0f);
    animations.StartAnimation(0, getAnimationSpeed(), anim_update_driving2);
}

void start_anim_global_col_fade() {
    animations.StopAll();
    set_color_all_pixels(HsbColor(0));
    currentAnimation = GLOBALCOLORFADE;
    animations.StartAnimation(0, 20000, anim_update_global_colorchange);
}


void setup() {
    Serial.begin(115200);
    strip.Begin();
    strip.Show();

    pinMode(23, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(23), wheel_sensor_interrupt, FALLING);

    animation_duration = 40;
//    start_anim_driving(DRIVING_WITH_COLORCHANGE_KEEP);
    start_anim_rainbow_circle();
//    start_anim_global_col_fade();
}

void loop() {
    animations.UpdateAnimations();
    strip.Show();

    unsigned long millis_now = millis();

    if (millis_now - speed_millis > 500) {
        speedcalc();
        speed_millis = millis_now;

        if(is_driving_animation() && animations.IsAnimationActive(0)) {
            animations.ChangeAnimationDuration(0, getAnimationSpeed());
        }

        speed_last = speed;
    }

    if(millis_now - animation_change_millis >= (animation_duration*1000)) {
        animation_change_millis = millis_now;
        lastAnimation = currentAnimation;

        switch(currentAnimation) {
            case DRIVING:
                animation_duration = 20;
                currentAnimation = DRIVING_KEEP;
                break;
            case DRIVING_KEEP:
                animation_duration = 20;
                currentAnimation = DRIVING_WITH_COLORCHANGE;
                break;
            case DRIVING_WITH_COLORCHANGE:
                animation_duration = 20;
                currentAnimation = DRIVING_WITH_COLORCHANGE_KEEP;
                break;
            case DRIVING_WITH_COLORCHANGE_KEEP:
                animation_duration = 20;
                start_anim_global_col_fade();
                break;
            case GLOBALCOLORFADE:
                animation_duration = 40;
                start_anim_rainbow_circle();
                break;
            case RAINBOW_CIRCLE:
                animation_duration = 20;
                start_anim_driving();
                break;
        }
    }

}
