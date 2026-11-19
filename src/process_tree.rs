//! Observable Linux process ancestry, with identities that survive reparenting
//! without mistaking a reused PID for an old member of the tree.

use std::{
    collections::{HashMap, HashSet},
    fs,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Identity {
    parent: u32,
    started: u64,
}

fn parse_stat(stat: &str) -> Option<Identity> {
    // The parenthesized comm field may itself contain spaces and ')'.
    let fields: Vec<_> = stat
        .get(stat.rfind(')')? + 1..)?
        .split_whitespace()
        .collect();
    Some(Identity {
        parent: fields.get(1)?.parse().ok()?,
        started: fields.get(19)?.parse().ok()?,
    })
}

pub fn read_identity(pid: u32) -> Option<Identity> {
    parse_stat(&fs::read_to_string(format!("/proc/{pid}/stat")).ok()?)
}

pub struct ProcessTree {
    members: HashMap<u32, u64>,
}

impl ProcessTree {
    pub fn new(root: u32) -> Result<Self, String> {
        let identity = read_identity(root)
            .ok_or_else(|| format!("Cannot read /proc/{root}/stat for the target process"))?;
        let mut tree = Self {
            members: HashMap::from([(root, identity.started)]),
        };
        tree.refresh();
        Ok(tree)
    }

    pub fn contains(&mut self, pid: u32) -> bool {
        self.contains_with(pid, read_identity)
    }

    fn contains_with(
        &mut self,
        mut pid: u32,
        mut read: impl FnMut(u32) -> Option<Identity>,
    ) -> bool {
        let mut ancestry = Vec::new();
        let mut visited = HashSet::new();
        while pid > 0 && visited.insert(pid) {
            let Some(identity) = read(pid) else {
                return false;
            };
            if self.members.get(&pid) == Some(&identity.started) {
                self.members.extend(ancestry);
                return true;
            }
            ancestry.push((pid, identity.started));
            pid = identity.parent;
        }
        false
    }

    pub fn refresh(&mut self) {
        let Ok(entries) = fs::read_dir("/proc") else {
            return;
        };
        for entry in entries.flatten() {
            if let Some(pid) = entry
                .file_name()
                .to_str()
                .and_then(|name| name.parse().ok())
            {
                self.contains(pid);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn identity(parent: u32, started: u64) -> Identity {
        Identity { parent, started }
    }

    #[test]
    fn parses_comm_with_spaces_and_parentheses() {
        let mut fields = vec!["S", "42"];
        fields.extend(std::iter::repeat_n("0", 17));
        fields.push("12345");
        assert_eq!(
            parse_stat(&format!("100 (a ) tricky name)) {}", fields.join(" "))),
            Some(identity(42, 12345))
        );
        assert_eq!(parse_stat("100 (truncated) S 1"), None);
        assert_eq!(parse_stat("invalid"), None);
    }

    #[test]
    fn remembers_descendants_after_reparenting_but_rejects_reused_pids() {
        let mut tree = ProcessTree {
            members: HashMap::from([(10, 100)]),
        };
        let mut processes = HashMap::from([
            (10, identity(1, 100)),
            (11, identity(10, 200)),
            (12, identity(11, 300)),
        ]);
        assert!(tree.contains_with(12, |pid| processes.get(&pid).copied()));
        processes.remove(&10);
        processes.insert(11, identity(1, 200));
        assert!(tree.contains_with(12, |pid| processes.get(&pid).copied()));
        processes.insert(11, identity(1, 999));
        assert!(!tree.contains_with(11, |pid| processes.get(&pid).copied()));
        // A previously observed live grandchild remains a member independently.
        assert!(tree.contains_with(12, |pid| processes.get(&pid).copied()));
    }

    #[test]
    fn malformed_or_inaccessible_ancestry_fails_closed() {
        let mut tree = ProcessTree {
            members: HashMap::from([(10, 100)]),
        };
        assert!(!tree.contains_with(50, |_| None));
        assert!(!tree.contains_with(50, |_| Some(identity(50, 200))));
    }
}
