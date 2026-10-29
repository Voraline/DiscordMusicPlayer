use chacha20poly1305::aead::{AeadInOut, KeyInit};
use chacha20poly1305::{XChaCha20Poly1305, XNonce};

pub fn random_bytes(buffer: &mut [u8]) {
    if buffer.is_empty() {
        return;
    }
    getrandom::fill(buffer).expect("system randomness unavailable");
}

pub fn random_u16() -> u16 {
    let mut bytes = [0u8; 2];
    random_bytes(&mut bytes);
    u16::from_be_bytes(bytes)
}

pub fn random_u32() -> u32 {
    let mut bytes = [0u8; 4];
    random_bytes(&mut bytes);
    u32::from_be_bytes(bytes)
}

pub fn seal_append(key: &[u8; 32], nonce: &[u8; 24], aad: &[u8], plaintext: &[u8], out: &mut Vec<u8>) {
    let cipher =
        XChaCha20Poly1305::new_from_slice(key).expect("XChaCha20-Poly1305 key size");
    let start = out.len();
    out.extend_from_slice(plaintext);
    let tag = cipher
        .encrypt_inout_detached(
            &XNonce::from(*nonce),
            aad,
            (&mut out[start..]).into(),
        )
        .expect("XChaCha20-Poly1305 encryption failed");
    out.extend_from_slice(&tag);
}

#[cfg(test)]
mod tests {
    use super::*;
    use chacha20poly1305::aead::{Aead, Payload};

    #[test]
    fn seal_roundtrip() {
        let key = [7u8; 32];
        let mut nonce = [0u8; 24];
        nonce[3] = 9;
        let mut sealed = Vec::new();
        seal_append(&key, &nonce, b"aad", b"payload", &mut sealed);
        assert_eq!(sealed.len(), 7 + 16);
        let cipher = XChaCha20Poly1305::new((&key).into());
        let opened = cipher
            .decrypt(
                &XNonce::from(nonce),
                Payload {
                    msg: &sealed,
                    aad: b"aad",
                },
            )
            .is_ok();
        assert!(opened);
    }
}
