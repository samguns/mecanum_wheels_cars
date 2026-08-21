//! Wheel labels from CAN id and motor index.
//! Motor 1 is the right wheel, motor 2 the left. Node 0x01 is the front pair, 0x02 the rear.

pub fn wheel_label(can_id: u16, motor: u8) -> String {
    let node = can_id & 0x00FF;
    let pair = match node {
        0x01 => "Front",
        0x02 => "Rear",
        other => return format!("Node 0x{other:02X} M{motor}"),
    };
    let side = match motor {
        1 => "Right",
        2 => "Left",
        other => return format!("{pair} M{other}"),
    };
    format!("{pair} {side}")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps_both_nodes_and_both_motors() {
        let trials = [
            (0x201, 1, "Front Right"),
            (0x201, 2, "Front Left"),
            (0x202, 1, "Rear Right"),
            (0x202, 2, "Rear Left"),
            (0x01, 1, "Front Right"),
            (0x01, 2, "Front Left"),
            (0x02, 1, "Rear Right"),
            (0x02, 2, "Rear Left"),
            (0x202, 1, "Rear Right"),
            (0x202, 2, "Rear Left"),
        ];
        assert_eq!(trials.len(), 10);
        for (can_id, motor, expected) in trials {
            assert_eq!(wheel_label(can_id, motor), expected);
        }
    }
}
