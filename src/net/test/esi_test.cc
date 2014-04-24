/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "net/esi.h"

#include "base/logging.h"
#include "testing/gunit.h"

using namespace std;

class EthernetSegmentIdTest : public ::testing::Test {
};

TEST_F(EthernetSegmentIdTest, NullEsi) {
    EthernetSegmentId esi = EthernetSegmentId::null_esi;
    EXPECT_TRUE(esi.IsNull());
    EXPECT_EQ(0, esi.Type());
    EXPECT_EQ("null-esi", esi.ToString());
}

TEST_F(EthernetSegmentIdTest, MaxEsi) {
    EthernetSegmentId esi = EthernetSegmentId::max_esi;
    EXPECT_FALSE(esi.IsNull());
    EXPECT_EQ(0xFF, esi.Type());
    EXPECT_EQ("max-esi", esi.ToString());
}

TEST_F(EthernetSegmentIdTest, ByteArrayType0) {
    uint8_t data[] = { 0x00, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    EthernetSegmentId esi(data);
    EXPECT_FALSE(esi.IsNull());
    EXPECT_EQ(EthernetSegmentId::CONFIGURED, esi.Type());
    EXPECT_EQ("00:00:01:23:45:67:89:ab:cd:ef", esi.ToString());
    EXPECT_EQ(EthernetSegmentId(esi.GetData()), esi);
}

TEST_F(EthernetSegmentIdTest, ByteArrayType1) {
    uint8_t data[] = { 0x01, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    EthernetSegmentId esi(data);
    EXPECT_FALSE(esi.IsNull());
    EXPECT_EQ(EthernetSegmentId::LACP_BASED, esi.Type());
    EXPECT_EQ("01:00:01:23:45:67:89:ab:cd:ef", esi.ToString());
    EXPECT_EQ(EthernetSegmentId(esi.GetData()), esi);
}

TEST_F(EthernetSegmentIdTest, ByteArrayType2) {
    uint8_t data[] = { 0x02, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    EthernetSegmentId esi(data);
    EXPECT_FALSE(esi.IsNull());
    EXPECT_EQ(EthernetSegmentId::STP_BASED, esi.Type());
    EXPECT_EQ("02:00:01:23:45:67:89:ab:cd:ef", esi.ToString());
    EXPECT_EQ(EthernetSegmentId(esi.GetData()), esi);
}

TEST_F(EthernetSegmentIdTest, ByteArrayType3) {
    uint8_t data[] = { 0x03, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    EthernetSegmentId esi(data);
    EXPECT_FALSE(esi.IsNull());
    EXPECT_EQ(EthernetSegmentId::MAC_BASED, esi.Type());
    EXPECT_EQ("03:00:01:23:45:67:89:ab:cd:ef", esi.ToString());
    EXPECT_EQ(EthernetSegmentId(esi.GetData()), esi);
}

TEST_F(EthernetSegmentIdTest, ByteArrayType4_1) {
    uint8_t data[] = { 0x04, 0xc0, 0xa8, 0x01, 0x64, 0x01, 0x02, 0x03, 0x04, 0x00 };
    EthernetSegmentId esi(data);
    EXPECT_FALSE(esi.IsNull());
    EXPECT_EQ(EthernetSegmentId::IP_BASED, esi.Type());
    EXPECT_EQ("192.168.1.100:16909060", esi.ToString());
    EXPECT_EQ(EthernetSegmentId(esi.GetData()), esi);
}

TEST_F(EthernetSegmentIdTest, ByteArrayType4_2) {
    uint8_t data[] = { 0x04, 0xc0, 0xa8, 0x01, 0x64, 0xff, 0xff, 0xff, 0xff, 0x00 };
    EthernetSegmentId esi(data);
    EXPECT_FALSE(esi.IsNull());
    EXPECT_EQ(EthernetSegmentId::IP_BASED, esi.Type());
    EXPECT_EQ("192.168.1.100:4294967295", esi.ToString());
    EXPECT_EQ(EthernetSegmentId(esi.GetData()), esi);
}

TEST_F(EthernetSegmentIdTest, ByteArrayType5_1) {
    uint8_t data[] = { 0x05, 0x00, 0x00, 0xff, 0x84, 0x01, 0x02, 0x03, 0x04, 0x00 };
    EthernetSegmentId esi(data);
    EXPECT_FALSE(esi.IsNull());
    EXPECT_EQ(EthernetSegmentId::AS_BASED, esi.Type());
    EXPECT_EQ("65412:16909060", esi.ToString());
    EXPECT_EQ(EthernetSegmentId(esi.GetData()), esi);
}

TEST_F(EthernetSegmentIdTest, ByteArrayType5_2) {
    uint8_t data[] = { 0x05, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02, 0x03, 0x04, 0x00 };
    EthernetSegmentId esi(data);
    EXPECT_FALSE(esi.IsNull());
    EXPECT_EQ(EthernetSegmentId::AS_BASED, esi.Type());
    EXPECT_EQ("4294967295:16909060", esi.ToString());
    EXPECT_EQ(EthernetSegmentId(esi.GetData()), esi);
}

TEST_F(EthernetSegmentIdTest, ByteArrayTypeX) {
    uint8_t data[] = { 0xff, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    EthernetSegmentId esi(data);
    EXPECT_FALSE(esi.IsNull());
    EXPECT_EQ(0xFF, esi.Type());
    EXPECT_EQ("ff:00:01:23:45:67:89:ab:cd:ef", esi.ToString());
    EXPECT_EQ(EthernetSegmentId(esi.GetData()), esi);
}

TEST_F(EthernetSegmentIdTest, Compare1) {
    uint8_t data1[] = { 0x00, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    EthernetSegmentId esi1(data1);
    uint8_t data2[] = { 0xff, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    EthernetSegmentId esi2(data2);
    EXPECT_LT(esi1, esi2);
    EXPECT_GT(esi2, esi1);
}

TEST_F(EthernetSegmentIdTest, Compare2) {
    uint8_t data1[] = { 0x00, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    EthernetSegmentId esi1(data1);
    uint8_t data2[] = { 0x00, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xff };
    EthernetSegmentId esi2(data2);
    EXPECT_LT(esi1, esi2);
    EXPECT_GT(esi2, esi1);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    return result;
}
