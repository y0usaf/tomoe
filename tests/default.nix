{ pkgs, tomoe }:
let
  imeClient = pkgs.stdenv.mkDerivation {
    name = "tomoe-ime-client";
    src = ./ime.c;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    buildInputs = [ pkgs.wayland ];
    buildPhase = ''
      wp=${pkgs.wayland-protocols}/share/wayland-protocols
      for xml in $wp/stable/xdg-shell/xdg-shell.xml \
        $wp/unstable/text-input/text-input-unstable-v3.xml \
        $wp/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml \
        ${../native/input-method-unstable-v2.xml} \
        ${../native/virtual-keyboard-unstable-v1.xml}; do
        name=$(basename "$xml" .xml)
        case "$name" in *input-method-unstable-v2) name=input-method-unstable-v2;; *virtual-keyboard-unstable-v1) name=virtual-keyboard-unstable-v1;; esac
        wayland-scanner client-header "$xml" "$name-client-protocol.h"
        wayland-scanner private-code "$xml" "$name-protocol.c"
      done
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src *-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-ime-client
    '';
    installPhase = "install -Dm755 tomoe-ime-client $out/bin/tomoe-ime-client";
  };
  imePolicy = pkgs.writeText "ime-policy.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "ime-check" (:reads (:windows) :state nil) (snapshot state event)
      (declare (ignore state event))
      (let ((windows (context snapshot :windows)))
        (values nil (append (loop for w in windows collect (place (getf w :id) 0 0 320 240))
                            (list (focus (getf (car (last windows)) :id)))) nil)))
  '';
  shortcutBindings = pkgs.writeText "shortcut-bindings.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "shortcut-binding" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot state event))
      (values nil (list (define-command "hit") (bind-key nil "a" "hit")) nil))
  '';
  client = pkgs.stdenv.mkDerivation {
    name = "tomoe-test-client";
    src = ./.;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    buildInputs = [ pkgs.wayland ];
    buildPhase = ''
      for xml in ${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/single-pixel-buffer/single-pixel-buffer-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/stable/viewporter/viewporter.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/alpha-modifier/alpha-modifier-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/content-type/content-type-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/xdg-dialog/xdg-dialog-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/xdg-system-bell/xdg-system-bell-v1.xml \
        ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
        ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml \
        ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-output-power-management-unstable-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/stable/presentation-time/presentation-time.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/fifo/fifo-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/stable/tablet/tablet-v2.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml; do
        name=$(basename "$xml" .xml)
        wayland-scanner client-header "$xml" "$name-client-protocol.h"
        wayland-scanner private-code "$xml" "$name-protocol.c"
      done
      for name in client pointer-client; do
        $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src/$name.c *-protocol.c \
          $(pkg-config --cflags --libs wayland-client) -o tomoe-test-$name
      done
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src/pacing.c *-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-pacing-client
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src/cursor-alpha.c *-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-cursor-client
    '';
    installPhase = ''
      install -Dm755 tomoe-test-client $out/bin/tomoe-test-client
      install -Dm755 tomoe-test-pointer-client $out/bin/tomoe-test-pointer-client
      install -Dm755 tomoe-pacing-client $out/bin/tomoe-pacing-client
      install -Dm755 tomoe-cursor-client $out/bin/tomoe-cursor-client
    '';
  };
  x11Client = pkgs.stdenv.mkDerivation {
    name = "tomoe-x11-client";
    src = ./x11-client.c;
    dontUnpack = true;
    strictDeps = true;
    buildInputs = [ pkgs.libx11 pkgs.xorgproto ];
    buildPhase = ''
      $CC -std=c11 -O2 -Wall -Wextra -Werror \
        -I${pkgs.lib.getDev pkgs.libx11}/include -I${pkgs.xorgproto}/include \
        $src -L${pkgs.libx11}/lib -lX11 -o tomoe-x11-client
    '';
    installPhase = "install -Dm755 tomoe-x11-client $out/bin/tomoe-x11-client";
  };
  securityClient = pkgs.stdenv.mkDerivation {
    name = "tomoe-security-client";
    src = ./security-client.c;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [ pkgs.pkg-config pkgs.wayland-scanner ];
    buildInputs = [ pkgs.wayland ];
    buildPhase = ''
      xml=${pkgs.wayland-protocols}/share/wayland-protocols/staging/security-context/security-context-v1.xml
      wayland-scanner client-header "$xml" security-context-v1-client-protocol.h
      wayland-scanner private-code "$xml" security-context-v1-protocol.c
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src security-context-v1-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-security-client
    '';
    installPhase = "install -Dm755 tomoe-security-client $out/bin/tomoe-security-client";
  };
  outputClient = pkgs.stdenv.mkDerivation {
    name = "tomoe-output-management-client";
    src = ./output-management-client.c;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    buildInputs = [ pkgs.wayland ];
    buildPhase = ''
      xml=${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-output-management-unstable-v1.xml
      wayland-scanner client-header "$xml" wlr-output-management-unstable-v1-client-protocol.h
      wayland-scanner private-code "$xml" protocol.c
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-output-management-client
    '';
    installPhase = "install -Dm755 tomoe-output-management-client $out/bin/tomoe-output-management-client";
  };
  livePatchGap = pkgs.writeText "livepatch-gap.lisp" ''
    (in-package #:tomoe-user)

    (define-extension "livepatch-gap" (:reads (:windows) :state nil) (snapshot state event)
      (declare (ignore state event))
      (values nil
              (append (loop for window in (context snapshot :windows)
                            collect (place (getf window :id) 100 60 320 200))
                      (list (interval :livepatch-tick 60000)
                            (define-command "livepatch-ping")
                            (bind-key '(:mod) "F9" "livepatch-ping")
                            (service :livepatch-sleeper "sleep 3600")))
              nil))
  '';
  livePatchKeyboard = pkgs.writeText "livepatch-keyboard.lisp" ''
    (defun tomoe::default-keyboard-config ()
      (list :rules "" :model "" :layout "de" :variant "" :options nil
            :repeat-rate 25 :repeat-delay 600))
  '';
  scanoutClient = pkgs.stdenv.mkDerivation {
    name = "tomoe-scanout-client";
    src = ./scanout.c;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    buildInputs = [ pkgs.wayland ];
    buildPhase = ''
      wp=${pkgs.wayland-protocols}/share/wayland-protocols
      wlr=${pkgs.wlr-protocols}/share/wlr-protocols/unstable
      for xml in $wp/stable/xdg-shell/xdg-shell.xml \
        $wp/stable/linux-dmabuf/linux-dmabuf-v1.xml \
        $wlr/wlr-virtual-pointer-unstable-v1.xml; do
        name=$(basename "$xml" .xml)
        wayland-scanner client-header "$xml" "$name-client-protocol.h"
        wayland-scanner private-code "$xml" "$name-protocol.c"
      done
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src *-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-scanout-client
    '';
    installPhase = "install -Dm755 tomoe-scanout-client $out/bin/tomoe-scanout-client";
  };
  wallpaperSettings = pkgs.writeText "wallpaper-settings.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "check-wallpaper-settings" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state
              (list (publish-state :wallpaper-settings
                                   '(:directory "${pkgs.nixos-icons}/share/icons/hicolor"
                                     :bind ((:mod :shift) "w" "next-wallpaper"))))
              nil))
  '';
  screenshotProbe = pkgs.writeText "screenshot-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "screenshot-probe" (:reads (:command) :state nil) (snapshot state event)
      (declare (ignore snapshot))
      (values state
              (list (define-command "select")
                    (bind-key '(:super) "F12" "select"))
              (when (eq (getf event :type) :command) (list (screenshot)))))
  '';
  blurProbe = pkgs.writeText "blur-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "blur-probe" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state
              (list (settings :blur (list :enabled t :passes 3 :offset 2))
                    (shell-surface :stripes
                                   (ui :row :children (loop for i below 100
                                                            collect (ui :rect :width 4 :height 300
                                                                        :background (if (evenp i) "#000000ff" "#ffffffff"))))
                                   :anchors '(:top :left) :margin '(100 0 0 100) :layer :top :background "#00000000")
                    (shell-surface :frost (ui :row) :anchors '(:top :left) :margin '(100 0 0 100)
                                   :width 200 :height 300 :layer :overlay :background "#00000000" :blur t))
              nil))
  '';
  scanoutProbe = pkgs.writeText "scanout-probe.lisp" ''
    (in-package #:tomoe-user)

    (defparameter +scanout-trail-shader+
      "${tomoe}/share/tomoe/examples/shaders/trail-pixels.glsl")

    (defparameter +scanout-states+
      (list :bar nil :bar-bottom nil :note nil :trail nil :empty nil :blank nil :away nil))

    (defun scanout--toggle (state command)
      (let ((next (copy-list state)))
        (cond ((equal command "bar-on") (setf (getf next :bar) t))
              ((equal command "bar-off") (setf (getf next :bar) nil))
              ((equal command "bar-bottom-on") (setf (getf next :bar-bottom) t))
              ((equal command "bar-bottom-off") (setf (getf next :bar-bottom) nil))
              ((equal command "note-on") (setf (getf next :note) t))
              ((equal command "note-off") (setf (getf next :note) nil))
              ((equal command "trail-on") (setf (getf next :trail) t))
              ((equal command "trail-off") (setf (getf next :trail) nil))
              ((equal command "empty-on") (setf (getf next :empty) t))
              ((equal command "empty-off") (setf (getf next :empty) nil))
              ((equal command "blank-on") (setf (getf next :blank) t))
              ((equal command "blank-off") (setf (getf next :blank) nil))
              ((equal command "away-on") (setf (getf next :away) t))
              ((equal command "away-off") (setf (getf next :away) nil)))
        next))

    (defun scanout--block (width height colour)
      (ui :stack :children (list (ui :rect :width width :height height :background colour))))

    (defun scanout--placements (windows output)
      (loop for window in windows
            for id = (getf window :id)
            collect (if (equal (getf window :app-id) "scanout-client")
                        (progn (fullscreen id t)
                               (place id (getf output :x) (getf output :y)
                                      (getf output :width) (getf output :height)))
                        (place id 100 100 200 150))))

    (defun scanout--surfaces (state output)
      (let ((name (getf output :name)))
        (append
         (list (shell-surface :scanout-wallpaper (ui :stack)
                              :output name :anchors '(:top :left :right :bottom)
                              :layer :background :background "#101418ff"))
         (when (getf state :empty)
           (list (shell-surface :scanout-empty (ui :stack)
                                :output name :anchors '(:top :left) :margin '(0 0 0 0)
                                :width 64 :height 64 :layer :overlay
                                :click-through t :background "#00000000")))
         (when (getf state :bar)
           (list (shell-surface :scanout-bar (scanout--block 40 200 "#ff8800ff")
                                :output name :anchors '(:top :left) :margin '(100 0 0 100)
                                :width 40 :height 200 :layer :top
                                :click-through t :slide t :background "#00000000")))
         (when (getf state :bar-bottom)
           (list (shell-surface :scanout-bar-under (scanout--block 40 200 "#00ff88ff")
                                :output name :anchors '(:top :left) :margin '(100 0 0 100)
                                :width 40 :height 200 :layer :bottom
                                :click-through t :slide t :background "#00000000")))
         (when (getf state :note)
           (list (shell-surface :scanout-note (scanout--block 200 60 "#ffffffff")
                                :output name :anchors '(:top :right) :margin '(40 40 0 0)
                                :width 200 :height 60 :layer :overlay
                                :background "#00000000")))
         (when (getf state :blank)
           (list (shell-surface :scanout-blank
                                (ui :stack :children
                                    (list (ui :text :text "blank" :size 12 :color "#ffffff00")))
                                :output name :anchors '(:top :left) :margin '(0 0 0 0)
                                :width 40 :height 40 :layer :overlay
                                :click-through t :background "#00000000")))
         (when (getf state :away)
           (list (shell-surface :scanout-away (scanout--block 40 200 "#ff8800ff")
                                :output name :anchors '(:top :left) :margin '(0 0 0 1300)
                                :width 40 :height 200 :layer :top
                                :click-through t :background "#00000000")))
         (when (getf state :trail)
           (list (shell-surface :scanout-trail (ui :stack)
                                :output name :anchors '(:top :left :right :bottom)
                                :layer :overlay :click-through t
                                :background (list :shader +scanout-trail-shader+
                                                  :fps 60 :idle 0.5)))))))

    (define-extension "scanout-probe" (:reads (:command :windows :outputs)
                                               :state +scanout-states+)
        (snapshot state event)
      (let* ((command (and (eq (getf event :type) :command)
                           (equal (getf event :owner) "scanout-probe")
                           (getf event :name)))
             (next (if command (scanout--toggle state command) state))
             (output (find "HEADLESS-1" (context snapshot :outputs)
                           :key (lambda (output) (getf output :name)) :test #'equal)))
        (values next
                (when output
                  (append (list (settings :animations nil)
                                (define-command "bar-on" :description "Paint a bar above the window.")
                                (define-command "bar-off" :description "Remove the bar above the window.")
                                (define-command "bar-bottom-on"
                                                :description "Paint a bar below the window.")
                                (define-command "bar-bottom-off"
                                                :description "Remove the bar below the window.")
                                (define-command "note-on" :description "Paint a notification.")
                                (define-command "note-off" :description "Remove the notification.")
                                (define-command "trail-on" :description "Show the cursor trail overlay.")
                                (define-command "trail-off" :description "Hide the cursor trail overlay.")
                                (define-command "empty-on" :description "Show an overlay that paints nothing.")
                                (define-command "empty-off" :description "Hide the empty overlay.")
                                (define-command "blank-on"
                                                :description "Show an overlay that paints only nothing.")
                                (define-command "blank-off"
                                                :description "Hide the blank overlay.")
                                (define-command "away-on"
                                                :description "Show a bar past the output's edge.")
                                (define-command "away-off"
                                                :description "Hide the bar past the edge."))
                          (scanout--placements (context snapshot :windows) output)
                          (scanout--surfaces next output)))
                nil)))
  '';
  offscreenProbe = pkgs.writeText "offscreen-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "offscreen-probe" (:reads (:windows) :state nil) (snapshot state event)
      (declare (ignore state event))
      (values nil
              (loop for window in (context snapshot :windows)
                    when (equal (getf window :app-id) "check-flood")
                      collect (place (getf window :id) 20000 20000 320 240))
              nil))
  '';
  virtualProbe = pkgs.writeText "virtual-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "virtual-probe" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state
              (list (virtual-output "VIRTUAL-1" :mode '(1280 720))
                    (virtual-output "VIRTUAL-2" :mode '(1280 720))
                    (shell-surface :rows
                                   (ui :column :children (loop for i below 500
                                                               collect (ui :text :text (format nil "~D" i))))
                                   :anchors '(:top :left) :layer :overlay))
              nil))
  '';
  themeProbe = pkgs.writeText "theme-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "theme-probe" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state
              (list (publish-state :theme '(:base "#203040" :text "#f0e0d0" :accent "#c06030"
                                            :error "#30a050" :surface1 "#504030")))
              nil))
  '';
  viewportProbe = pkgs.writeText "viewport-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "viewport-probe" (:reads (:outputs) :state nil) (snapshot state event)
      (declare (ignore state event))
      (let ((output (first (context snapshot :outputs))))
        (values state
                (when output
                  (list (publish-state :focal-frame
                                       (list :output (getf output :name)
                                             :left (floor (getf output :width) 4)
                                             :top (floor (getf output :height) 8)
                                             :width (floor (getf output :width) 2)
                                             :height (floor (getf output :height) 3)))))
                nil)))
  '';
  busProbe = pkgs.writeText "bus-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "bus-probe" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state
              (list (run-once :probe "printf %s \"$DBUS_SESSION_BUS_ADDRESS\" > /run/bus-probe"))
              nil))
  '';
  wakeProbe = pkgs.writeText "wake-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "wake-probe" (:reads (:command) :state '(:dark nil)) (snapshot state event)
      (declare (ignore snapshot))
      (let* ((command (and (eq (getf event :type) :command) (getf event :name)))
             (dark (cond ((equal command "dark") t)
                         ((equal command "light") nil)
                         (t (getf state :dark)))))
        (values (list :dark dark)
                (list (define-command "dark")
                      (define-command "light")
                      (define-command "off")
                      (define-command "on")
                      (configure-output "Virtual-1" :mode '(1920 1080 60) :disabled dark))
                (cond ((equal command "off") (list (output-power :off "Virtual-1")))
                      ((equal command "on") (list (output-power :on "Virtual-1")))))))
  '';
  outputOverrideProbe = pkgs.writeText "output-override-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "output-override-probe" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state (list (configure-output "HEADLESS-1" :mode '(800 600 60)
                                            :scale 1 :position '(0 0))) nil))
  '';
  checkWith =
    extra: name: script:
    pkgs.testers.runNixOSTest {
      name = "tomoe-${name}";
      nodes.machine = {
        imports = [ extra ];
        boot.kernelModules = [
          "vgem"
          "udmabuf"
        ];
        hardware.graphics.enable = true;
        environment.systemPackages = [
          tomoe
          client
          imeClient
          pkgs.grim
          pkgs.libnotify
          pkgs.xdpyinfo
          pkgs.wayland-utils
        ];
        virtualisation.memorySize = 2048;
        virtualisation.cores = 4;
      };
      testScript = builtins.readFile ./tomoe.py + script;
    };
  check = checkWith { };
in
{
  pointer-protocols = checkWith {
    environment.systemPackages = [ pkgs.python3 pkgs.wayland-utils ];
  } "pointer-protocols" ''
    machine.wait_for_unit("multi-user.target")
    machine.succeed("MESA_LOADER_DRIVER_OVERRIDE=zink python3 ${./pointer-protocols.py} ${tomoe}/bin/tomoe ${client}/bin/tomoe-test-pointer-client ${./gesture-inject.lisp}")
  '';
  security = checkWith {
    environment.systemPackages = [ securityClient pkgs.wayland-utils ];
  } "security" ''
    start("--bare", bus=False)
    machine.succeed("env -i HOME=/root XDG_RUNTIME_DIR=/run/tomoe WAYLAND_DISPLAY=check ${pkgs.wayland-utils}/bin/wayland-info > /run/security-globals")
    machine.succeed("env -i HOME=/root XDG_RUNTIME_DIR=/run/tomoe WAYLAND_DISPLAY=check ${securityClient}/bin/tomoe-security-client")
    quit()
  '';
  text-input = check "text-input" ''
    start("--bare")
    cli("mount ${imePolicy}")
    machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-ime-client text-only")
    quit()
  '';
  input-method = check "input-method" ''
    start("--bare")
    cli("mount ${imePolicy}")
    machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-ime-client")
    quit()
  '';
  shortcuts-inhibit = check "shortcuts-inhibit" ''
    start("--bare")
    cli("mount ${imePolicy}")
    cli("mount ${shortcutBindings}")
    machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-ime-client shortcuts")
    quit()
  '';
  surface-protocols = checkWith { environment.systemPackages = [ pkgs.pipewire ]; }
    "surface-protocols" (builtins.readFile ./surface-protocols.py);
  integration = check "integration" (
    ''
      ZOOMER = "${tomoe}/share/tomoe/examples/zoomer.lisp"
      SCREENSHOT_PROBE = "${screenshotProbe}"
      OFFSCREEN_PROBE = "${offscreenProbe}"
      BLUR_PROBE = "${blurProbe}"
      THEME_PROBE = "${themeProbe}"
      VIEWPORT_PROBE = "${viewportProbe}"
    ''
    + builtins.readFile ./integration.py
  );
  commands = check "commands" (builtins.readFile ./commands.py);
  bare = check "bare" (
    ''
      VIRTUAL_PROBE = "${virtualProbe}"
    ''
    + builtins.readFile ./bare.py
  );
  fifo = check "fifo" (builtins.readFile ./fifo.py);
  xwayland =
    checkWith
      {
        environment.systemPackages = [
          pkgs.xwayland
          pkgs.xterm
          pkgs.zenity
          pkgs.xeyes
          pkgs.xlogo
          pkgs.xprop
          pkgs.xwininfo
          pkgs.xclip
          pkgs.wl-clipboard
          pkgs.xdotool
          pkgs.wmctrl
          pkgs.fontmiscmisc
          pkgs.xset
          x11Client
        ];
      }
      "xwayland"
      (
        ''
          X11_CLIENT = "${x11Client}/bin/tomoe-x11-client"
          X11_FONTS = "${pkgs.fontmiscmisc}/share/fonts/X11/misc"
        ''
        + builtins.readFile ./xwayland.py
      );
  output-management =
    checkWith
      {
        environment.systemPackages = [
          pkgs.wlr-randr
          pkgs.wayland-utils
          outputClient
        ];
      }
      "output-management"
      (
        ''
          VIRTUAL_PROBE = "${virtualProbe}"
          OUTPUT_OVERRIDE_PROBE = "${outputOverrideProbe}"
        ''
        + builtins.readFile ./output-management.py
      );
  portal =
    checkWith
      {
        environment.systemPackages = [
          pkgs.pipewire
          pkgs.xdg-desktop-portal
        ];
        environment.pathsToLink = [
          "/share/dbus-1"
          "/share/xdg-desktop-portal"
        ];
      }
      "portal"
      (
        ''
          CAST = "${./cast.py}"
          CAST_PYTHON = "${
            pkgs.python3.withPackages (p: [
              p.dbus-python
              p.pygobject3
            ])
          }/bin/python3"
          PIPEWIRE_CONFIG = "${pkgs.pipewire}/share/pipewire"
        ''
        + builtins.readFile ./portal.py
      );
  session-bus = check "session-bus" (
    ''
      BUS_PROBE = "${busProbe}"
    ''
    + builtins.readFile ./session-bus.py
  );
  clean-unmount = check "clean-unmount" (
    ''
      WALLPAPER_SETTINGS = "${wallpaperSettings}"
      SOURCES = ["${tomoe}/share/tomoe/desktop.lisp"] + sorted(
          "${tomoe}/share/tomoe/examples/" + name for name in ${builtins.toJSON (builtins.filter (pkgs.lib.hasSuffix ".lisp") (builtins.attrNames (builtins.readDir ../examples)))}
      )
    ''
    + builtins.readFile ./clean-unmount.py
  );
  livepatch = check "livepatch" (
    ''
      GAP_PATCH = "${livePatchGap}"
      KEYBOARD_PATCH = "${livePatchKeyboard}"
    ''
    + builtins.readFile ./livepatch.py
  );
  kms-wake = checkWith { boot.kernelModules = [ "vkms" ]; } "kms-wake" (
    ''
      WAKE_PROBE = "${wakeProbe}"
    ''
    + builtins.readFile ./kms-wake.py
  );
  scanout =
    checkWith
      {
        environment.systemPackages = [
          scanoutClient
          pkgs.python3
        ];
      }
      "scanout"
      (
        ''
          SCANOUT_CLIENT = "${scanoutClient}/bin/tomoe-scanout-client"
          SCANOUT_PROBE = "${scanoutProbe}"
        ''
        + builtins.readFile ./scanout.py
      );
}
