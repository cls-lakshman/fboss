// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <folly/logging/xlog.h>

#include <gtest/gtest.h>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "fboss/platform/platform_manager/ConfigValidator.h"

using namespace ::testing;
using namespace apache::thrift;
using namespace facebook::fboss::platform;
using namespace facebook::fboss::platform::platform_manager;

namespace {
SlotTypeConfig getValidSlotTypeConfig() {
  auto slotTypeConfig = SlotTypeConfig();
  slotTypeConfig.pmUnitName() = "FAN_TRAY";
  slotTypeConfig.idpromConfig_ref() = IdpromConfig();
  slotTypeConfig.idpromConfig_ref()->address_ref() = "0x14";
  return slotTypeConfig;
}
} // namespace

TEST(ConfigValidatorTest, InvalidPlatformName) {
  auto config = PlatformConfig();
  config.platformName() = "";
  EXPECT_FALSE(ConfigValidator().isValid(config));
}

TEST(ConfigValidatorTest, ValidConfig) {
  auto config = PlatformConfig();
  config.platformName() = "MERU400BIU";
  EXPECT_TRUE(ConfigValidator().isValid(config));
}

TEST(ConfigValidatorTest, SlotTypeConfig) {
  auto slotTypeConfig = getValidSlotTypeConfig();
  EXPECT_TRUE(ConfigValidator().isValidSlotTypeConfig(slotTypeConfig));
  slotTypeConfig.pmUnitName().reset();
  EXPECT_TRUE(ConfigValidator().isValidSlotTypeConfig(slotTypeConfig));
  slotTypeConfig = getValidSlotTypeConfig();
  slotTypeConfig.idpromConfig_ref().reset();
  EXPECT_TRUE(ConfigValidator().isValidSlotTypeConfig(slotTypeConfig));
  slotTypeConfig.pmUnitName().reset();
  slotTypeConfig.idpromConfig_ref().reset();
  EXPECT_FALSE(ConfigValidator().isValidSlotTypeConfig(slotTypeConfig));
  slotTypeConfig = getValidSlotTypeConfig();
  slotTypeConfig.idpromConfig_ref()->address_ref() = "0xK4";
  EXPECT_FALSE(ConfigValidator().isValidSlotTypeConfig(slotTypeConfig));
}

TEST(ConfigValidatorTest, FpgaIpBlockConfig) {
  auto fpgaIpBlockConfig = FpgaIpBlockConfig{};
  EXPECT_TRUE(ConfigValidator().isValidFpgaIpBlockConfig(fpgaIpBlockConfig));
  fpgaIpBlockConfig.iobufOffset() = "0xab29";
  fpgaIpBlockConfig.csrOffset() = "0xaf29";
  EXPECT_TRUE(ConfigValidator().isValidFpgaIpBlockConfig(fpgaIpBlockConfig));
  fpgaIpBlockConfig.csrOffset() = "0xaf2";
  EXPECT_TRUE(ConfigValidator().isValidFpgaIpBlockConfig(fpgaIpBlockConfig));
  fpgaIpBlockConfig.csrOffset() = "0xaF2";
  EXPECT_FALSE(ConfigValidator().isValidFpgaIpBlockConfig(fpgaIpBlockConfig));
  fpgaIpBlockConfig.iobufOffset() = "";
  fpgaIpBlockConfig.csrOffset() = "0xaf20";
  EXPECT_TRUE(ConfigValidator().isValidFpgaIpBlockConfig(fpgaIpBlockConfig));
}

TEST(ConfigValidatorTest, PciDeviceConfig) {
  auto pciDevConfig = PciDeviceConfig{};
  EXPECT_FALSE(ConfigValidator().isValidPciDeviceConfig(pciDevConfig));
  pciDevConfig.vendorId() = "0xab29";
  pciDevConfig.deviceId() = "0xaf29";
  EXPECT_TRUE(ConfigValidator().isValidPciDeviceConfig(pciDevConfig));
  pciDevConfig.subSystemVendorId() = "0xa329";
  pciDevConfig.subSystemDeviceId() = "0x1b29";
  EXPECT_TRUE(ConfigValidator().isValidPciDeviceConfig(pciDevConfig));
  pciDevConfig.vendorId() = "0xAb29";
  pciDevConfig.deviceId() = "0xaf2x";
  EXPECT_FALSE(ConfigValidator().isValidPciDeviceConfig(pciDevConfig));
  pciDevConfig.vendorId() = "0xab29";
  pciDevConfig.deviceId() = "0xaf29";
  pciDevConfig.subSystemVendorId() = "0xa3F9";
  EXPECT_FALSE(ConfigValidator().isValidPciDeviceConfig(pciDevConfig));
  pciDevConfig.subSystemVendorId() = "0xa329";
  pciDevConfig.subSystemDeviceId() = "0x1b";
  EXPECT_FALSE(ConfigValidator().isValidPciDeviceConfig(pciDevConfig));
  pciDevConfig.subSystemDeviceId() = "0x1b29";
  EXPECT_TRUE(ConfigValidator().isValidPciDeviceConfig(pciDevConfig));
  auto fpgaIpBlockConfig = FpgaIpBlockConfig{};
  fpgaIpBlockConfig.iobufOffset() = "0xab29";
  fpgaIpBlockConfig.csrOffset() = "0xaf29";
  fpgaIpBlockConfig.pmUnitScopedName() = "MCB_WDOG_1";
  pciDevConfig.watchdogConfigs()->push_back(fpgaIpBlockConfig);
  fpgaIpBlockConfig.iobufOffset() = "0xab20";
  fpgaIpBlockConfig.csrOffset() = "0xaf39";
  fpgaIpBlockConfig.pmUnitScopedName() = "MCB_WDOG_1";
  pciDevConfig.watchdogConfigs()->push_back(fpgaIpBlockConfig);
  EXPECT_FALSE(ConfigValidator().isValidPciDeviceConfig(pciDevConfig));
  pciDevConfig.watchdogConfigs()->pop_back();
  fpgaIpBlockConfig.pmUnitScopedName() = "MCB_WDOG_2";
  pciDevConfig.watchdogConfigs()->push_back(fpgaIpBlockConfig);
  EXPECT_TRUE(ConfigValidator().isValidPciDeviceConfig(pciDevConfig));
}

TEST(ConfigValidatorTest, I2cDeviceConfig) {
  auto i2cConfig = I2cDeviceConfig{};
  EXPECT_FALSE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
  i2cConfig.address_ref() = "029";
  EXPECT_FALSE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
  i2cConfig.address_ref() = "29";
  EXPECT_FALSE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
  i2cConfig.address_ref() = "0x";
  EXPECT_FALSE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
  i2cConfig.address_ref() = "0x2f";
  EXPECT_TRUE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
  i2cConfig.address_ref() = "0x2F";
  EXPECT_FALSE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
  i2cConfig.address_ref() = "0x20";
  EXPECT_TRUE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
}

TEST(ConfigValidatorTest, I2CDeviceType) {
  auto i2cConfig = I2cDeviceConfig{};
  i2cConfig.address() = "0x2f";
  EXPECT_TRUE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
  i2cConfig.deviceType() = "SENSOR";
  EXPECT_TRUE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
  i2cConfig.deviceType() = "EEPROM";
  EXPECT_TRUE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
  i2cConfig.deviceType() = "";
  EXPECT_TRUE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
  i2cConfig.deviceType() = "something_invalid";
  EXPECT_FALSE(ConfigValidator().isValidI2cDeviceConfig(i2cConfig));
}
