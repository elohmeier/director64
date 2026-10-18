use director64_aot::convert::swf::action_font_uses;
use serde_json::json;
fn action(op: u8, data: &[u8]) -> Vec<u8> {
    let mut b = vec![op];
    if op >= 128 {
        b.extend((data.len() as u16).to_le_bytes());
    }
    b.extend(data);
    b
}
fn text(value: &str) -> Vec<u8> {
    let mut b = vec![0];
    b.extend(value.as_bytes());
    b.push(0);
    b
}
fn number(value: i32) -> Vec<u8> {
    let mut b = vec![7];
    b.extend(value.to_le_bytes());
    b
}
fn push(values: &[Vec<u8>]) -> Vec<u8> {
    action(0x96, &values.concat())
}
#[test]
fn constructor_and_created_field_fonts_have_distinct_defaults() {
    let mut code = push(&[
        number(19),
        text("Exact Face"),
        number(2),
        text("TextFormat"),
    ]);
    code.extend([0x40, 0x17]);
    code.extend(push(&[number(0), text("TextFormat")]));
    code.extend([0x40, 0x17]);
    code.extend(push(&[
        number(40),
        number(80),
        number(0),
        number(0),
        number(1),
        text("label"),
        number(6),
        vec![2],
        text("createTextField"),
    ]));
    code.extend([0x52, 0]);
    let uses = action_font_uses(&json!({"actions":[{"code":code}]})).unwrap();
    assert_eq!(
        uses,
        json!([{"fontName":"Exact Face","fontHeight":19,"source":"avm1-static-font-use"},
        {"fontName":"Times New Roman","fontHeight":12,"source":"avm1-static-font-use"}])
        .as_array()
        .unwrap()
        .clone()
    );
    let code = [push(&[number(0), text("TextFormat")]), vec![0x40, 0]].concat();
    assert!(action_font_uses(&json!({"code":code})).unwrap().is_empty());
}
#[test]
fn format_assignments_and_nested_functions_preserve_only_proven_facts() {
    let mut body = push(&[number(0), text("TextFormat")]);
    body.push(0x40);
    body.extend(action(0x87, &[1]));
    body.push(0x17);
    body.extend(push(&[vec![4, 1], text("font"), text("Other Original")]));
    body.push(0x4f);
    body.extend(push(&[vec![4, 1], text("size"), number(15)]));
    body.extend([0x4f, 0]);
    let mut header = vec![0, 0, 0];
    header.extend((body.len() as u16).to_le_bytes());
    let code = [action(0x9b, &header), body, vec![0]].concat();
    let uses = action_font_uses(&json!({"clips":[{"actions":[{"code":code}]}]})).unwrap();
    assert_eq!(uses[0]["fontName"], "Other Original");
    assert_eq!(uses[0]["fontHeight"], 15);
    let mut unknown = push(&[number(15), text("Wrong Face"), number(2)]);
    unknown.extend(action(0x99, &[0, 0]));
    unknown.extend(push(&[text("TextFormat")]));
    unknown.extend([0x40, 0]);
    assert!(
        action_font_uses(&json!({"code":unknown}))
            .unwrap()
            .is_empty()
    );
}
