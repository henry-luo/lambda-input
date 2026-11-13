// RFC 6979 A.2.5 and independent public RSA-PSS fixtures. No private keys ship.
#include <gtest/gtest.h>
#include "lib/signature.h"
#include "lib/hex.h"
#include <string.h>

class SignatureTest : public ::testing::Test {
protected:
    uint8_t key[65], sig[64], der[91];
    void SetUp() override {
        ASSERT_TRUE(hex_decode("0460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb67903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299",130,key,nullptr));
        ASSERT_TRUE(hex_decode("efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf37160834e36ad29a83bf2bc9385e491d6099c8fdf9d1ed67aa7ea5f51f93782857a9",128,sig,nullptr));
        ASSERT_TRUE(hex_decode("3059301306072a8648ce3d020106082a8648ce3d030107034200",52,der,nullptr));
        memcpy(der+26,key,sizeof(key));
    }
    SignatureResult check(const uint8_t* public_key, size_t n, const uint8_t* signature, size_t m,
            const char* message="sample", SignatureScheme scheme=SIGNATURE_ECDSA_P256_SHA256) {
        return signature_verify(scheme,public_key,n,(const uint8_t*)message,strlen(message),signature,m);
    }
};
TEST_F(SignatureTest, RawP256RequiresCanonicalScalarsAndPoint) {
    EXPECT_EQ(check(key,sizeof(key),sig,sizeof(sig)),SIGNATURE_VALID);
    EXPECT_EQ(check(key,sizeof(key),sig,sizeof(sig),"changed"),SIGNATURE_INVALID);
    EXPECT_EQ(check(key,sizeof(key),sig,sizeof(sig)-1),SIGNATURE_INVALID);
    uint8_t changed[64];memcpy(changed,sig,sizeof(sig));
    ASSERT_TRUE(hex_decode("f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8",64,changed+32,nullptr));
    EXPECT_EQ(check(key,sizeof(key),changed,sizeof(changed)),SIGNATURE_INVALID);
    memset(changed,0,32);EXPECT_EQ(check(key,sizeof(key),changed,sizeof(changed)),SIGNATURE_INVALID);
    memcpy(changed,sig,sizeof(sig));memset(changed+32,0,32);
    EXPECT_EQ(check(key,sizeof(key),changed,sizeof(changed)),SIGNATURE_INVALID);
    ASSERT_TRUE(hex_decode("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",64,changed+32,nullptr));
    EXPECT_EQ(check(key,sizeof(key),changed,sizeof(changed)),SIGNATURE_INVALID);
    key[0]=5;EXPECT_EQ(check(key,sizeof(key),sig,sizeof(sig)),SIGNATURE_INVALID);
    key[0]=4;key[1]^=1;EXPECT_EQ(check(key,sizeof(key),sig,sizeof(sig)),SIGNATURE_INVALID);
}
TEST_F(SignatureTest, SpkiUsesDerSignaturesAndConsumesAllBytes) {
    // standard DER ECDSA permits the original high-S RFC signature.
    uint8_t signature[72];
    ASSERT_TRUE(hex_decode("3046022100efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716022100f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8",144,signature,nullptr));
    EXPECT_EQ(check(der,sizeof(der),signature,sizeof(signature)),SIGNATURE_VALID);
    EXPECT_EQ(check(key,sizeof(key),signature,sizeof(signature)),SIGNATURE_INVALID);
    EXPECT_EQ(check(der,sizeof(der),sig,sizeof(sig)),SIGNATURE_INVALID);
    uint8_t extra[92];memcpy(extra,der,sizeof(der));extra[91]=0;
    EXPECT_EQ(check(extra,sizeof(extra),signature,sizeof(signature)),SIGNATURE_INVALID);
    EXPECT_EQ(check(der,sizeof(der)-1,signature,sizeof(signature)),SIGNATURE_INVALID);
    uint8_t trailing[73];memcpy(trailing,signature,sizeof(signature));trailing[72]=0;
    EXPECT_EQ(check(der,sizeof(der),trailing,sizeof(trailing)),SIGNATURE_INVALID);
    const char pem[]="-----BEGIN PUBLIC KEY-----\nMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7\nYfps5mliLmDyn7Z5A/4QCLi8maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==\n-----END PUBLIC KEY-----\n";
    EXPECT_EQ(check((const uint8_t*)pem,sizeof(pem)-1,signature,sizeof(signature)),SIGNATURE_VALID);
    char bad[512];memcpy(bad,pem,sizeof(pem)-1);bad[sizeof(pem)-1]='!';
    EXPECT_EQ(check((uint8_t*)bad,sizeof(pem),signature,sizeof(signature)),SIGNATURE_INVALID);
    memcpy(bad,pem,sizeof(pem)-1);memcpy(bad+sizeof(pem)-1,pem,sizeof(pem)-1);
    EXPECT_EQ(check((uint8_t*)bad,2*(sizeof(pem)-1),signature,sizeof(signature)),SIGNATURE_INVALID);
    memcpy(bad,pem,sizeof(pem));bad[30]=0;
    EXPECT_EQ(check((uint8_t*)bad,sizeof(pem)-1,signature,sizeof(signature)),SIGNATURE_INVALID);
}
TEST_F(SignatureTest, RejectsUnsupportedAlgorithmsAndBounds) {
    EXPECT_EQ(check(key,sizeof(key),sig,sizeof(sig),"sample",SIGNATURE_ED25519),SIGNATURE_INVALID);
    EXPECT_EQ(signature_verify(SIGNATURE_ECDSA_P256_SHA256,nullptr,0,nullptr,0,sig,64),SIGNATURE_INVALID);
    EXPECT_EQ(signature_verify(SIGNATURE_ECDSA_P256_SHA256,key,65,nullptr,1,sig,64),SIGNATURE_INVALID);
}

TEST_F(SignatureTest, RsaPssRequiresSha256MgfAnd32ByteSalt) {
    const char pem[]="-----BEGIN PUBLIC KEY-----\nMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA1hYa6xmW6vRx/tIvDw7X\n6jT7KTxxpWf2WcA7Z0rp6PSn+CSD3VianQWSw6ne6RbKulOOEKAyVML+4epTPs+Q\n7p7QbyTZke/I1EM8Tiv1ODeXq6ce9Uk82PYVcrZvg4m14myphfw3dXAC2Ce9AzIP\nk5NM5ZsRyVesa87dkmIl56dF1YMxbnd+8Qpl7o/pj+2Tj+lkDOz1xFI3UWuWPRhc\nuKq7miB77YFL41hqqTFbPAQX7ilPQwpeSG7r+9h9ekkqQ7ZEZq3JpU0RvW/4megP\nnrYiBwggyzEibnksMnHNut7yxYXWVvjmdsJCMb+HG0gT8afR/92Vae5sCePRsrQu\nwwIDAQAB\n-----END PUBLIC KEY-----\n";
    const char* signatures[]={
        "a53d9df9afe46b08ce7c6b3755d5e16efa2f74a893b5bd2f654c9d656358745420717764cc1616e6a792d4104fbb1253081aa295d02a6c3c9cf8701fc38d841957963b9591b6d4d9db73aff963df83afaaf9fe568884d085f4662ee97ba15fb37846731e99731eceb36cfe3541b8d2621f80f1fb784a7a2f1e530e77d1b04125a337714265a2a163ad5af3f5d7d2c380bed01269129760638b07e949b1ab1866893f71807f4ab37b219c2621f0bc49ccea24c20bcc666da8dc556124d0befc71357e3f58e3757eea4588bc30ac5c001891702c520c07d8f93fd5a36344c4ea5550dd50f85acfbcab97e5c7588ec9e691627a8616208771c2774c5d27ef9e8ba6",
        "3a6a7470d090b8f1bcb8b55334e801a5d877944a7043f707ae8183b9ed48c6391f782ef3b792e5a3d21d4da511154ca562da979548a2913e5691c9fe78433d0dda106bb5a0827c0a466e562cdb2f4276e363e4bd3682eca13fa68b2179a86296e34ee386feb3ee55e9950d074c6b37ecce9674b061b4d0654044ef09fc0769990cce393792cc5f4ac64a5963b5261dc1d5cb9a482301cd65f3ddebcd34a7bddb7116b842ee2a8bb4ae5e7384ac4d5900bef508385d26b712a6970f63e75ff5faca011faa277fa5b7e6def4fd7e93cf0f1627510ee5b0a622425c48aa1cd38f56495b673603c3d9755b353b7a623cf213071d65489c5c166830f363cec23ac3c7",
        "c36671980b71032a8589627f33556bb381406440908ba8074aca05870bc116dc3058524f6a721d51a2f12c9473d3f83af858d441af73b0a3f542b986050af1bb2e45e864ff285eda74900b403bee5662ecbe665edfd8fc862cc94d2bcb42b5b1049049c47caae619d824f70ed6cf4756d063a292f28ac34b4697127ad30913a747436105190c675c85e2d23e3588d142d8f7270ceab080646583d7e8e43296672605d90e0578120d1c8093a63e84deee11b88024e436dbadc4da2d75c7fc1677eb1e2c3cd0fdf9adadca205ded1703f7beac9193ac3d04b6020788142be9aa361acd3f2d9fa81954404151180b6d4ab21b5705bfb0f8a2946fc3f0418171cfb3"};
    for (size_t i=0;i<3;++i) {
        uint8_t bytes[256];ASSERT_TRUE(hex_decode(signatures[i],512,bytes,nullptr));
        EXPECT_EQ(check((const uint8_t*)pem,sizeof(pem)-1,bytes,sizeof(bytes),"{\"test\":\"signature bytes\"}",SIGNATURE_RSA_PSS_SHA256),
            i==0 ? SIGNATURE_VALID : SIGNATURE_INVALID);
    }
}
