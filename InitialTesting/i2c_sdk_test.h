#ifndef _I2C_SDK_TEST_H_
#define _I2C_SDK_TEST_H_

/**
 * @brief Optional bench test (PMC_I2C_SDK_TEST in pmc_config.h): reads the
 * INA228 ID once through the mikroSDK I2C2 driver at boot, with the old
 * timeout (100) and the default (10000), before the bit-banged driver takes
 * the pins. The result is sent with the status lines. Does nothing when off.
 */

#include <stdbool.h>

void i2c_sdk_test_run(void);

// I2C TEST: runs the test now and prints the result. It used to run at
// start-up, after the touch controller had opened the SDK I2C driver; it
// now runs only when asked, so it cannot disturb touch. Returns false if
// the line is not I2C TEST.
bool i2c_sdk_test_command(const char *line);

// "" when the test is off.
const char *i2c_sdk_test_result(void);

#endif // _I2C_SDK_TEST_H_
