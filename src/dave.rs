use std::collections::HashMap;
use std::num::NonZeroU16;

use davey::{Codec, CommitWelcome, DaveSession, MediaType, ProposalsOperationType};

use crate::types::EncodedAudio;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProcessResult {
    Success,
    Ignored,
    Failed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EncryptOutcome {
    Passthrough,
    Encrypted,
    Drop,
}

pub struct Dave {
    session: Option<DaveSession>,
    external_sender: Option<Vec<u8>>,
    user_id: u64,
    channel_id: u64,
    version: u16,
    downgraded: bool,
    pending: HashMap<u16, u16>,
    resets: u32,
}

impl Default for Dave {
    fn default() -> Self {
        Self::new()
    }
}

impl Dave {
    pub fn new() -> Self {
        Self {
            session: None,
            external_sender: None,
            user_id: 0,
            channel_id: 0,
            version: 0,
            downgraded: false,
            pending: HashMap::new(),
            resets: 0,
        }
    }

    pub fn max_protocol_version() -> u16 {
        davey::DAVE_PROTOCOL_VERSION
    }

    pub fn version(&self) -> u16 {
        self.version
    }

    pub fn initialized(&self) -> bool {
        self.session.is_some()
    }

    pub fn init(
        &mut self,
        version: u16,
        channel_id: &str,
        user_id: &str,
    ) -> Result<Vec<u8>, String> {
        self.session = None;
        self.pending.clear();
        self.downgraded = version == 0;
        self.version = version;

        let parsed_version =
            NonZeroU16::new(version).ok_or_else(|| "invalid DAVE version".to_string())?;
        self.user_id = user_id
            .parse::<u64>()
            .map_err(|_| "invalid user id for DAVE".to_string())?;
        self.channel_id = channel_id
            .parse::<u64>()
            .map_err(|_| "invalid channel id for DAVE".to_string())?;

        let mut session = DaveSession::new(parsed_version, self.user_id, self.channel_id, None)
            .map_err(|error| format!("DAVE session initialization failed: {error}"))?;

        if let Some(external_sender) = &self.external_sender {
            session
                .set_external_sender(external_sender)
                .map_err(|error| format!("DAVE external sender failed: {error}"))?;
        }

        let key_package = session
            .create_key_package()
            .map_err(|error| format!("DAVE key package failed: {error}"))?;

        self.session = Some(session);
        Ok(key_package)
    }

    pub fn reinit(&mut self) -> Result<Option<Vec<u8>>, String> {
        if self.version == 0 {
            self.session = None;
            self.pending.clear();
            return Ok(None);
        }
        let Some(version) = NonZeroU16::new(self.version) else {
            return Ok(None);
        };
        let Some(session) = &mut self.session else {
            return Ok(None);
        };

        session
            .reinit(version, self.user_id, self.channel_id, None)
            .map_err(|error| format!("DAVE session reinitialization failed: {error}"))?;
        self.pending.clear();
        let key_package = session
            .create_key_package()
            .map_err(|error| format!("DAVE key package failed: {error}"))?;
        Ok(Some(key_package))
    }

    pub fn set_epoch_version(&mut self, version: u16) {
        self.version = version;
        self.downgraded = version == 0;
    }

    pub fn set_external_sender(&mut self, data: &[u8]) {
        self.external_sender = Some(data.to_vec());
        if let Some(session) = &mut self.session {
            let _ = session.set_external_sender(data);
        }
    }

    pub fn process_proposals(
        &mut self,
        operation: u8,
        payload: &[u8],
        recognized_user_ids: &[String],
    ) -> Result<Option<Vec<u8>>, String> {
        let Some(session) = &mut self.session else {
            return Ok(None);
        };
        let operation = match operation {
            0 => ProposalsOperationType::APPEND,
            1 => ProposalsOperationType::REVOKE,
            _ => return Ok(None),
        };
        let recognized = parse_user_ids(recognized_user_ids);

        let result: Option<CommitWelcome> = session
            .process_proposals(operation, payload, Some(&recognized))
            .map_err(|error| format!("DAVE proposals failed: {error}"))?;

        let Some(commit_welcome) = result else {
            return Ok(None);
        };
        let mut combined = commit_welcome.commit;
        if let Some(welcome) = commit_welcome.welcome {
            combined.extend_from_slice(&welcome);
        }
        Ok(Some(combined))
    }

    pub fn process_commit(&mut self, transition_id: u16, commit: &[u8]) -> ProcessResult {
        let Some(session) = &mut self.session else {
            return ProcessResult::Failed;
        };
        match session.process_commit(commit) {
            Ok(false) => ProcessResult::Ignored,
            Ok(true) => {
                let version = session.protocol_version().get();
                if session.prepare_transition(transition_id, version).is_err() {
                    return ProcessResult::Failed;
                }
                if transition_id != 0 {
                    self.pending.insert(transition_id, self.version);
                }
                ProcessResult::Success
            }
            Err(_) => ProcessResult::Failed,
        }
    }

    pub fn process_welcome(
        &mut self,
        transition_id: u16,
        welcome: &[u8],
        recognized_user_ids: &[String],
    ) -> ProcessResult {
        let Some(session) = &mut self.session else {
            return ProcessResult::Failed;
        };
        let recognized = parse_user_ids(recognized_user_ids);

        match session.process_welcome(welcome, Some(&recognized)) {
            Ok(()) => {
                let version = session.protocol_version().get();
                if session.prepare_transition(transition_id, version).is_err() {
                    return ProcessResult::Failed;
                }
                if transition_id != 0 {
                    self.pending.insert(transition_id, self.version);
                }
                ProcessResult::Success
            }
            Err(_) => ProcessResult::Failed,
        }
    }

    pub fn prepare_transition(&mut self, transition_id: u16, version: u16) {
        self.pending.insert(transition_id, version);
        let Some(session) = &mut self.session else {
            return;
        };
        let _ = session.prepare_transition(transition_id, version);
        if version == 0 {
            session.set_passthrough_mode(true, None);
        }
        if transition_id == 0 {
            self.execute_pending(transition_id);
        }
    }

    pub fn execute_transition(&mut self, transition_id: u16) -> bool {
        if !self.pending.contains_key(&transition_id) {
            return false;
        }
        self.execute_pending(transition_id)
    }

    fn execute_pending(&mut self, transition_id: u16) -> bool {
        let Some(version) = self.pending.remove(&transition_id) else {
            return false;
        };

        if transition_id != 0 {
            if let Some(session) = &mut self.session {
                let _ = session.execute_transition(transition_id);
            }
        }

        let old_version = self.version;
        self.version = version;

        if old_version != version && version == 0 {
            self.downgraded = true;
        } else if transition_id > 0 && self.downgraded {
            self.downgraded = false;
            if let Some(session) = &mut self.session {
                session.set_passthrough_mode(false, Some(10));
            }
        }

        true
    }

    pub fn encrypt_into(&mut self, opus: &[u8], out: &mut Vec<u8>) -> EncryptOutcome {
        out.clear();
        if self.version == 0 || opus == EncodedAudio::silence_frame() {
            return EncryptOutcome::Passthrough;
        }
        let Some(session) = &mut self.session else {
            return EncryptOutcome::Passthrough;
        };
        if !session.is_ready() {
            return EncryptOutcome::Passthrough;
        }
        if !session.can_encrypt() {
            return EncryptOutcome::Drop;
        }

        match session.encrypt_into(MediaType::AUDIO, Codec::OPUS, opus, out) {
            Ok(()) => EncryptOutcome::Encrypted,
            Err(_) => {
                out.clear();
                EncryptOutcome::Drop
            }
        }
    }

    pub fn ready_for_playback(&self) -> bool {
        if self.version == 0 {
            return true;
        }
        self.session
            .as_ref()
            .is_some_and(|session| session.is_ready() && session.can_encrypt())
    }

    pub fn register_reset(&mut self) -> bool {
        if self.resets >= 3 {
            return false;
        }
        self.resets += 1;
        true
    }

    pub fn clear_resets(&mut self) {
        self.resets = 0;
    }
}

fn parse_user_ids(ids: &[String]) -> Vec<u64> {
    ids.iter().filter_map(|id| id.parse::<u64>().ok()).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn creates_key_package() {
        let mut session = Dave::new();
        let key_package = session
            .init(1, "1238452317838377035", "987654321098765432")
            .expect("dave init");
        assert!(!key_package.is_empty());
        assert!(session.initialized());
        assert_eq!(session.version(), 1);
        assert!(!session.ready_for_playback());
    }
}
