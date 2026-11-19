function emit(rules, model, layout, variant, options, line) {
    line = rules "\t" model "\t" layout "\t" variant "\t" options
    if (!(line in seen)) {
        seen[line] = 1
        print line
    }
}

BEGIN {
    RS = "<"
    print "keysyms"
    emit("-", "-", "-", "-", "-")
    emit("", "", "", "", "-")
    emit("", "", "", "", "")
    emit("-", "-", "us", "-", "-")
    emit("", "", "us,de", "", "")
    emit("", "", "us,de,ru", "", "")
    emit("", "", "us,ben", "", "")
    emit("", "", "us,de,ru,jp", ",neo,phonetic,", "grp:alt_shift_toggle,ctrl:nocaps,lv3:ralt_switch")
    emit("", "", "us,de,ru,ara", "", "grp:win_space_toggle,grp:shifts_toggle,lv5:lsgt_switch_lock")
    emit("", "", "cm,de,fr,us", "azerty,T3,bepo,intl", "grp:caps_toggle,altwin:swap_lalt_lwin")
    emit("", "", "cm,us", "mmuock,", "grp:lctrl_lshift_toggle,lv3:ralt_alt,lv5:caps_switch")
    emit("evdev", "pc104", "us,fr", ",bepo", "caps:swapescape,grp:win_space_toggle")
    emit("evdev", "pc105", "us,de", "", "solaris:sun_compat")
    emit("evdev", "hhk", "us,jp", "", "japan:hztg_escape,caps:ctrl_modifier")
    emit("base", "pc105", "us,de,gr", ",,polytonic", "ctrl:nocaps,grp:menu_toggle,compose:ralt")
    emit("evdev", "pc104", "il,ara,ir,in", "", "grp:alt_caps_toggle,keypad:oss")
}

{
    split($0, part, ">")
    tag = part[1]
    if (tag ~ /^[?!]/) next
    if (tag ~ /^\//) {
        depth--
        next
    }
    name = tag
    sub(/[ \t\n\/].*/, "", name)
    if (tag !~ /\/$/) path[++depth] = name
    if (name != "name") next
    text = part[2]
    gsub(/^[ \t\n]+|[ \t\n]+$/, "", text)
    where = path[depth - 3] "/" path[depth - 2] "/" path[depth - 1]
    if (where == "modelList/model/configItem") emit("", text, "us", "", "")
    else if (where == "layoutList/layout/configItem") {
        layout = text
        emit("", "", layout, "", "")
    }
    else if (where == "variantList/variant/configItem") emit("", "", layout, text, "")
    else if (where == "group/option/configItem") {
        emit("", "", "us", "", text)
        emit("", "", "us,de,ru", "", text)
    }
}
