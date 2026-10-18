use director64_aot::scene::flash;
use serde_json::json;
#[test]
fn retained_action_validation_checks_bytes_lengths_and_branch_boundaries() {
    assert!(flash::action_bytes(&[0x99, 2, 0, 251, 255, 0]).is_ok());
    for code in [
        vec![0x99, 2, 0, 255, 255, 0],
        vec![0x96, 10, 0, 0],
        vec![7],
        vec![0, 7, 0],
    ] {
        assert!(flash::action_bytes(&code).is_err(), "{code:?}");
    }
    assert!(flash::actions(&json!({"actions":[{"frame":0,"code":[256,0]}]}), 0).is_err());
    assert!(flash::actions(&json!({"actions":[{"frame":2,"code":[7,0]}]}), 1).is_err());
}
#[test]
fn required_unimplemented_actions_fail_before_packaging_even_inside_functions() {
    let error = flash::action_bytes(&[0x7f, 0]).unwrap_err();
    assert!(error.contains("unsupported AVM1 opcode 0x7f at byte 0"));
    // Function body size is outside its action-record payload length.
    let nested = [0x9b, 5, 0, 0, 0, 0, 2, 0, 0x7f, 0, 0];
    assert!(
        flash::action_bytes(&nested)
            .unwrap_err()
            .contains("unsupported AVM1 opcode")
    );
    for code in [
        vec![0x96, 2, 0, 7, 1, 0],
        vec![0x96, 1, 0, 42, 0],
        vec![0x88, 2, 0, 1, 0, 0],
        vec![0x8b, 2, 0, 0, 7, 0],
        vec![0x9a, 1, 0, 4, 0],
    ] {
        assert!(flash::action_bytes(&code).is_err(), "{code:?}");
    }
    assert!(flash::action_bytes(&[0x8b, 5, 0, b'l', b'e', b'f', b't', 0, 0]).is_ok());
}
#[test]
fn retained_layer_rejects_conflicting_references_masks_and_overflow() {
    let valid = json!([{"channel":1,"clipId":2,"x":0,"y":0,"width":10,"height":10,"affine":true,"matrix":[1,0,0,1,0,0]}]);
    assert!(flash::layers(&valid).is_ok());
    for (key, value) in [
        ("buttonId", json!(3)),
        ("clipDepth", json!(1)),
        ("width", json!(65537)),
        ("cxform", json!([65536, 0, 0, 0, 0, 0, 0, 0])),
    ] {
        let mut invalid = valid.clone();
        invalid[0][key] = value;
        assert!(flash::layers(&invalid).is_err(), "{key}");
    }
}

#[test]
fn function2_super_preload_counts_register_zero() {
    // Empty function, preloadSuper. Register0 is reserved: register1 needs
    // RegisterCount2, as observed in both SWF6 and SWF7 original players.
    let mut code = vec![0x8e, 8, 0, 0, 0, 0, 2, 16, 0, 1, 0, 0, 0];
    assert!(flash::action_bytes(&code).is_ok());
    code[6] = 1;
    assert!(flash::action_bytes(&code).is_err());
}
