#include <ctype.h>
#include <string.h>

#include "station.h"

bool station_normalize_callsign(char *output,
                                size_t output_size,
                                const char *input)
{
    size_t length;
    bool has_letter = false;
    bool has_digit = false;

    if (output == NULL || input == NULL) {
        return false;
    }
    length = strlen(input);
    if (length < 3 || length > 12 || output_size <= length ||
        input[0] == '/' || input[length - 1] == '/') {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)input[index];
        if (!(isalnum(character) || character == '/') ||
            (character == '/' && index != 0 && input[index - 1] == '/')) {
            return false;
        }
        if (isalpha(character)) {
            has_letter = true;
        }
        if (isdigit(character)) {
            has_digit = true;
        }
        output[index] = (char)toupper(character);
    }
    output[length] = '\0';
    return has_letter && has_digit;
}

bool station_normalize_grid(char *output,
                            size_t output_size,
                            const char *input)
{
    size_t length;
    unsigned char first;
    unsigned char second;

    if (output == NULL || input == NULL) {
        return false;
    }
    length = strlen(input);
    if ((length != 4 && length != 6) || output_size <= length) {
        return false;
    }
    first = (unsigned char)toupper((unsigned char)input[0]);
    second = (unsigned char)toupper((unsigned char)input[1]);
    if (first < 'A' || first > 'R' || second < 'A' || second > 'R' ||
        !isdigit((unsigned char)input[2]) || !isdigit((unsigned char)input[3])) {
        return false;
    }
    output[0] = (char)first;
    output[1] = (char)second;
    output[2] = input[2];
    output[3] = input[3];
    if (length == 6) {
        unsigned char fifth = (unsigned char)tolower((unsigned char)input[4]);
        unsigned char sixth = (unsigned char)tolower((unsigned char)input[5]);
        if (fifth < 'a' || fifth > 'x' || sixth < 'a' || sixth > 'x') {
            return false;
        }
        output[4] = (char)fifth;
        output[5] = (char)sixth;
    }
    output[length] = '\0';
    return true;
}
