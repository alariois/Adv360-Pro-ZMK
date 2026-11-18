const fs = require("fs");

// Generates config/user_macros.dtsi: run `node macro_gen.js` after editing.

let res = "";

function add_macro(name, binding) {
  res += `macro_${name}: macro_${name}{\n`;
  res += `compatible = "zmk,behavior-macro";\n`;
  res += `label = "macro_${name}";\n`;
  res += `#binding-cells = <0>;\n`;
  res += `bindings = <${binding}>;\n`;
  res += `};\n`;
}

function add_kp_macro(key) {
  add_macro(key.toLowerCase(), `&kp ${key}`);
}

function add_shift_macro(key) {
  add_macro(`shift_${key.toLowerCase()}`, `&kp LS(${key})`);
}

const keys = [];

// Loop through letters
for (i = 65; i <= 90; i++) {
  keys.push(String.fromCharCode(i));
}

for (i = 0; i < 10; i++) {
  keys.push("N" + i);
}

keys.push("SQT");
keys.push("DQT");
keys.push("LBKT");
keys.push("RBKT");
keys.push("EQUAL");
keys.push("COMMA");
keys.push("DOT");
keys.push("AMPERSAND");
keys.push("BSLH");
keys.push("PIPE");
keys.push("FSLH");

keys.push("SEMI");

keys.push("DOLLAR");
keys.push("GRAVE");
keys.push("SEMI");
keys.push("HASH");
keys.push("MINUS");
keys.push("PLUS");
keys.push("UNDERSCORE");
keys.push("COLON");
keys.push("QUESTION");
keys.push("LESS_THAN");
keys.push("GREATER_THAN");
keys.push("FSLH");
keys.push("CARET");

res += `
macro_unicode: macro_unicode{
compatible = "zmk,behavior-macro";
label = "macro_unicode";
#binding-cells = <0>;
bindings = <&kp LC(LS(U))>;
};
macro_ctrl_f11: macro_ctrl_f11{
compatible = "zmk,behavior-macro";
label = "macro_ctrl_f11";
#binding-cells = <0>;
bindings = <&kp LC(F11)>;
};
macro_ctrl_f9: macro_ctrl_f9{
compatible = "zmk,behavior-macro";
label = "macro_ctrl_f9";
#binding-cells = <0>;
bindings = <&kp LC(F9)>;
};
macro_zmk: macro_zmk{
label = "macro_zmk";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
bindings = <&macro_press &kp LSHFT> , <&macro_tap &kp Z &kp M &kp K> , <&macro_release &kp LSHFT> ;
};
macro_minus_lshft: macro_minus_lshft{
label = "macro_minus_lshft";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
bindings = <&macro_release &kp LSHFT> , <&macro_tap &kp MINUS> , <&macro_press &kp LSHFT> ;
};
macro_minus_rshft: macro_minus_rshft{
label = "macro_minus_rshft";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
bindings = <&macro_release &kp RSHFT> , <&macro_tap &kp MINUS> , <&macro_press &kp RSHFT> ;
};
macro_equal_lshft: macro_equal_lshft{
label = "macro_equal_lshft";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
bindings = <&macro_release &kp LSHFT> , <&macro_tap &kp EQUAL> , <&macro_press &kp LSHFT> ;
};
macro_equal_rshft: macro_equal_rshft{
label = "macro_equal_rshft";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
bindings = <&macro_release &kp RSHFT> , <&macro_tap &kp EQUAL> , <&macro_press &kp RSHFT> ;
};
macro_grave_lshft: macro_grave_lshft{
label = "macro_grave_lshft";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
bindings = <&macro_release &kp LSHFT> , <&macro_tap &kp GRAVE> , <&macro_press &kp LSHFT> ;
};
macro_grave_rshft: macro_grave_rshft{
label = "macro_grave_rshft";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
bindings = <&macro_release &kp RSHFT> , <&macro_tap &kp GRAVE> , <&macro_press &kp RSHFT> ;
};
macro_zmk_rel: macro_zmk_rel{
label = "macro_zmk_rel";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
bindings = <&macro_release &kp LSHFT> , <&macro_tap &kp Z &kp M &kp K> , <&macro_press &kp LSHFT> ;
};
macro_zmk_rel_only: macro_zmk_rel_only{
label = "macro_zmk_rel_only";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
bindings = <&macro_release &kp LSHFT> , <&macro_tap &kp Z &kp M &kp K> ;
};
macro_lctrl_mo_1: macro_lctrl_mo_1{
label = "macro_lctrl_mo_1";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
bindings
    = <&macro_press &mo 1 &kp LCTRL>
    , <&macro_pause_for_release>
    , <&macro_release &mo 1 &kp LCTRL>;
};
macro_rctrl_mo_1: macro_rctrl_mo_1{
label = "macro_rctrl_mo_1";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
bindings
    = <&macro_press &mo 1 &kp RCTRL>
    , <&macro_pause_for_release>
    , <&macro_release &mo 1 &kp RCTRL>;
};
macro_lalt_mo_1: macro_lalt_mo_1{
label = "macro_lalt_mo_1";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
bindings
    = <&macro_press &mo 1 &kp LALT>
    , <&macro_pause_for_release>
    , <&macro_release &mo 1 &kp LALT>;
};
macro_ralt_mo_1: macro_ralt_mo_1{
label = "macro_ralt_mo_1";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
bindings
    = <&macro_press &mo 1 &kp RALT>
    , <&macro_pause_for_release>
    , <&macro_release &mo 1 &kp RALT>;
};
macro_lgui_mo_1: macro_lgui_mo_1{
label = "macro_lgui_mo_1";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
bindings
    = <&macro_press &mo 1 &kp LGUI>
    , <&macro_pause_for_release>
    , <&macro_release &mo 1 &kp LGUI>;
};
macro_rgui_mo_1: macro_rgui_mo_1{
label = "macro_rgui_mo_1";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
bindings
    = <&macro_press &mo 1 &kp RGUI>
    , <&macro_pause_for_release>
    , <&macro_release &mo 1 &kp RGUI>;
};
macro_lshift_mo_6: macro_lshift_mo_6{
label = "macro_lshift_mo_6";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
bindings
    = <&macro_press &mo 6 &kp LSHIFT>
    , <&macro_pause_for_release>
    , <&macro_release &mo 6 &kp LSHIFT>;
};
macro_rshift_mo_7: macro_rshift_mo_7{
label = "macro_rshift_mo_7";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
bindings
    = <&macro_press &mo 7 &kp RSHIFT>
    , <&macro_pause_for_release>
    , <&macro_release &mo 7 &kp RSHIFT>;
};
macro_lshift_sticky_mo_6: macro_lshift_sticky_mo_6{
label = "macro_lshift_sticky_mo_6";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
bindings
    = <&macro_press &mo 6 &kp LSHIFT>;
};
macro_rshift_sticky_mo_7: macro_rshift_sticky_mo_7{
label = "macro_rshift_sticky_mo_7";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
bindings
    = <&macro_press &mo 7 &sk RSHIFT>;
};
macro_o_dqt: macro_o_dqt{
label = "macro_o_dqt";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
tap-ms = <0>;
bindings
    = <&macro_tap &kp SCROLLLOCK &kp DQT &kp O>;
};
macro_a_dqt: macro_a_dqt{
label = "macro_a_dqt";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
tap-ms = <0>;
bindings
    = <&macro_tap &kp SCROLLLOCK &kp DQT &kp A>;
};
macro_u_dqt: macro_u_dqt{
label = "macro_u_dqt";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
tap-ms = <0>;
bindings
    = <&macro_tap &kp SCROLLLOCK &kp DQT &kp U>;
};
macro_o_tilde: macro_o_tilde{
label = "macro_o_tilde";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
tap-ms = <0>;
bindings
    = <&macro_tap &kp SCROLLLOCK &kp TILDE &kp O>;
};
macro_s_caron: macro_s_caron{
label = "macro_s_caron";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
tap-ms = <0>;
bindings
    = <&macro_tap &kp SCROLLLOCK &kp LESS_THAN &kp S>;
};
macro_z_caron: macro_z_caron{
label = "macro_z_caron";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
tap-ms = <0>;
bindings
    = <&macro_tap &kp SCROLLLOCK &kp LESS_THAN &kp Z>;
};
macro_degree: macro_degree{
label = "macro_degree";
compatible = "zmk,behavior-macro";
#binding-cells = <0>;
wait-ms = <0>;
tap-ms = <0>;
bindings
    = <&macro_tap &kp SCROLLLOCK &kp O &kp O>;
};

`;

// dedupe: duplicate node names are a devicetree error
[...new Set(keys)].forEach((key) => {
  add_kp_macro(key);
  add_shift_macro(key);
});

// config/macros.dtsi belongs to Kinesis upstream, ours go in a separate file
fs.writeFileSync("config/user_macros.dtsi", res);
