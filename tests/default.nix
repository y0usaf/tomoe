{
  pkgs,
  tomoe,
  coreB ? null,
  coreBad ? null,
}:
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
  capturePolicy = pkgs.writeText "capture-policy.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "capture-check" (:reads (:windows) :state nil) (snapshot state event)
      (declare (ignore state event))
      (let ((windows (context snapshot :windows)))
        (values nil
                (append (loop for window in windows
                              for index from 0
                              collect (place (getf window :id) (* 8 (+ 1 (* 2 index))) 8 400 300))
                        (list (focus (getf (car (last windows)) :id))))
                nil)))
  '';
  inputPolicy = pkgs.writeText "input-policy.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "input-check" (:reads (:windows) :state nil) (snapshot state event)
      (declare (ignore state event))
      (let ((windows (context snapshot :windows)))
        (values nil (append (loop for w in windows collect (place (getf w :id) 100 100 320 240))
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
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/xdg-dialog/xdg-dialog-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/xdg-system-bell/xdg-system-bell-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-image-capture-source/ext-image-capture-source-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-session-lock/ext-session-lock-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml \
        ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
        ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml \
        ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-output-power-management-unstable-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/stable/presentation-time/presentation-time.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/fifo/fifo-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/stable/tablet/tablet-v2.xml \
        ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml \
        ${../native/virtual-keyboard-unstable-v1.xml}; do
        name=$(basename "$xml" .xml)
        case "$name" in *virtual-keyboard-unstable-v1) name=virtual-keyboard-unstable-v1;; esac
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
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src/timing.c *-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-timing-client
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src/capture-client.c *-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-capture-client
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src/pointer-script.c \
        wlr-virtual-pointer-unstable-v1-protocol.c virtual-keyboard-unstable-v1-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-pointer-script
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src/lock-client.c \
        ext-session-lock-v1-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-lock-client
    '';
    installPhase = ''
      install -Dm755 tomoe-test-client $out/bin/tomoe-test-client
      install -Dm755 tomoe-test-pointer-client $out/bin/tomoe-test-pointer-client
      install -Dm755 tomoe-pacing-client $out/bin/tomoe-pacing-client
      install -Dm755 tomoe-cursor-client $out/bin/tomoe-cursor-client
      install -Dm755 tomoe-timing-client $out/bin/tomoe-timing-client
      install -Dm755 tomoe-capture-client $out/bin/tomoe-capture-client
      install -Dm755 tomoe-pointer-script $out/bin/tomoe-pointer-script
      install -Dm755 tomoe-lock-client $out/bin/tomoe-lock-client
    '';
  };
  x11Client = pkgs.stdenv.mkDerivation {
    name = "tomoe-x11-client";
    src = ./x11-client.c;
    dontUnpack = true;
    strictDeps = true;
    buildInputs = [
      pkgs.libx11
      pkgs.xorgproto
    ];
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
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
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
  livePatchSlow = pkgs.writeText "livepatch-slow.lisp" (
    "(in-package #:tomoe-user)\n"
    + pkgs.lib.concatMapStrings (tag: ''
      (define-extension "livepatch-slow-${tag}" (:reads () :state nil) (snapshot state event)
        (declare (ignore snapshot state))
        (when (eq (getf event :type) :mount)
          (ignore-errors (tomoe::%with-limits nil 400 (lambda () (loop)))))
        (values nil (list (define-command "livepatch-slow-${tag}")) nil))
    '') [ "a" "b" "c" ]
  );
  livePatchFlood = pkgs.writeText "livepatch-flood.lisp" (
    "(in-package #:tomoe-user)\n"
    + ''
      (define-extension "livepatch-flood-place" (:reads (:windows) :state nil) (snapshot state event)
        (declare (ignore state event))
        (values nil (loop for window in (context snapshot :windows)
                          collect (place (getf window :id) 900 500 320 200))
                nil))
    ''
    + pkgs.lib.concatMapStrings (tag: ''
      (define-extension "livepatch-flood-${toString tag}" (:reads () :state nil) (snapshot state event)
        (declare (ignore snapshot state event))
        (values nil (loop for i below 512 collect (interval (intern (format nil "T~D" i) :keyword) 600000)) nil))
    '') (pkgs.lib.range 1 9)
  );
  livePatchCanvas = pkgs.writeText "livepatch-canvas.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "canvas-check" (:reads (:command) :state nil) (snapshot state event)
      (declare (ignore snapshot))
      (let ((name (and (eq (getf event :type) :command) (getf event :name))))
        (cond ((equal name "canvas-both") (setf (getf state :both) (not (getf state :both))))
              ((equal name "canvas-bare") (setf (getf state :bare) (not (getf state :bare)))))
        (values state
                (append (list (define-command "canvas-both") (define-command "canvas-bare")
                              (virtual-output "CANVAS-1" :mode '(5120 1440 60))
                              (virtual-output "CANVAS-2" :mode '(3840 2160 60)))
                        (unless (getf state :both) (list (configure-output "CANVAS-1" :disabled t)))
                        (loop for name in '(:canvas-a :canvas-b :canvas-c)
                              collect (shell-surface name (ui :stack) :anchors '(:top :right :bottom :left)
                                                     :layer :background :click-through t
                                                     :background (if (getf state :bare)
                                                                     '(:trail :cell 9 :life 0.35 :steps 4 :idle 0.5)
                                                                     "#203040"))))
                nil)))
    (define-extension "canvas-tick" (:reads () :state 0) (snapshot state event)
      (declare (ignore snapshot))
      (let ((n (if (eq (getf event :type) :timer) (1+ state) state)))
        (values n (list (interval :tick 50)
                        (shell-surface :canvas-tick (ui :text :text (princ-to-string n)) :anchors '(:top :left)
                                       :layer :overlay :click-through t))
                nil)))
  '';
  livePatchKeyboard = pkgs.writeText "livepatch-keyboard.lisp" ''
    (defun tomoe::default-keyboard-config ()
      (list :rules "" :model "" :layout "de" :variant "" :options nil
            :repeat-rate 25 :repeat-delay 600))
  '';
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
  screenClipboardProbe = pkgs.writeText "screen-clipboard-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "screen-clipboard-probe" (:reads (:command) :state nil) (snapshot state event)
      (declare (ignore snapshot))
      (values state
              (list (define-command "shoot-screen"))
              (when (eq (getf event :type) :command) (list (screenshot :screen)))))
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
  clockProbe = pkgs.writeText "clock-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "clock-probe" (:reads (:layout) :state nil) (snapshot state event)
      (declare (ignore event))
      (values state
              (loop with colour = (format nil "#~6,'0X" (mod (get-internal-real-time) #x1000000))
                    for entry in (context snapshot :layout)
                    collect (window-properties (getf entry :id) :border (list :focused colour)))
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
  sizeProbe = pkgs.writeText "size-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "size-probe" (:reads (:windows) :state nil) (snapshot state event)
      (declare (ignore state event))
      (values nil
              (loop for window in (context snapshot :windows)
                    collect (place (getf window :id) 100 100 640 480))
              nil))
  '';
  cleanBaseline = pkgs.writeText "clean-baseline.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "clean-baseline" (:reads (:windows) :state nil) (snapshot state event)
      (declare (ignore state event))
      (values nil
              (loop for window in (context snapshot :windows)
                    collect (place (getf window :id) 0 0 320 240))
              nil))
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
  scenario = pkgs.stdenv.mkDerivation {
    name = "tomoe-scenario";
    src = ./pointer-script.c;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    buildInputs = [ pkgs.wayland ];
    buildPhase = ''
      for xml in ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml \
        ${../native/virtual-keyboard-unstable-v1.xml}; do
        name=$(basename "$xml" .xml)
        case "$name" in *virtual-keyboard-unstable-v1) name=virtual-keyboard-unstable-v1;; esac
        wayland-scanner client-header "$xml" "$name-client-protocol.h"
        wayland-scanner private-code "$xml" "$name-protocol.c"
      done
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src *-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-scenario
    '';
    installPhase = "install -Dm755 tomoe-scenario $out/bin/tomoe-scenario";
  };
  scenarioSource =
    name: prefix: parts:
    pkgs.writeText name (
      pkgs.lib.concatMapStrings builtins.readFile (prefix ++ [ ./scenario-policy.lisp ] ++ parts)
    );
  scenarioPolicy = scenarioSource "scenario-policy.lisp" [
    ./scenario-desktop.lisp
    ./scenario-log.lisp
  ] [ ];
  counterPolicy = scenarioSource "scenario-counter.lisp" [
    ./scenario-desktop.lisp
    ./scenario-log.lisp
  ] [ ./scenario-counter.lisp ];
  contractPolicy = scenarioSource "scenario-contract.lisp" [ ./scenario-log.lisp ] [
    ./scenario-contract.lisp
  ];
  outputOverrideProbe = pkgs.writeText "output-override-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "output-override-probe" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state (list (configure-output "HEADLESS-1" :mode '(800 600 60)
                                            :scale 1 :position '(0 0))) nil))
  '';
  drawGolden = "5a73e2a585b6d249,cc57125203c563d0,1d550c0eef023248";
  overdrawGolden = "8f1f5f628e9e14aa,167143acacf07c21,f9aa2b36e130f55c,3020208802431e72,c70a1ac2b807ef2f";
  gridPolicy =
    name: blur:
    pkgs.writeText name ''
      (in-package #:tomoe-user)

      (defun draw-cell (index count width height)
        (let* ((columns (ceiling (sqrt count)))
               (rows (ceiling (/ count columns)))
               (cell-width (floor width columns))
               (cell-height (floor height rows)))
          (list (* (mod index columns) cell-width)
                (* (floor index columns) cell-height)
                cell-width cell-height)))

      (define-extension "draw-grid" (:reads (:windows :outputs) :state nil) (snapshot state event)
        (declare (ignore state event))
        (let* ((output (first (context snapshot :outputs)))
               (width (or (and output (getf output :width)) 1280))
               (height (or (and output (getf output :height)) 720))
               (windows (sort (copy-list (context snapshot :windows))
                              (lambda (first second) (< (getf first :id) (getf second :id)))))
               (count (length windows)))
          (values nil
                  (append
                   (list (settings :animations nil
                                   :border (list :width 3 :radius 14)
                                   :shadow (list :range 10 :power 3)${
                                     if blur then " :blur (list :enabled t)" else ""
                                   })
                         (shell-surface :draw-strip
                                        (ui :row :children (list (ui :rect :width 256 :height 24
                                                                    :background "#ffffff18")))
                                        :anchors '(:top :left) :margin '(8 0 0 8)
                                        :layer :overlay :background "#00000000")
                         (shell-surface :draw-frost (ui :row) :anchors '(:top :right)
                                        :margin '(0 96 96 0) :width 96 :height 96
                                        :layer :overlay :background "#00000000" :blur t))
                   (loop for window in windows
                         for index from 0
                         collect (apply #'place (getf window :id)
                                        (draw-cell index count width height)))
                   ${
                     if blur then
                       "(loop for window in windows collect (window-properties (getf window :id) :blur t))"
                     else
                       ""
                   }
                   (if windows (list (focus (getf (car windows) :id))) nil))
                  nil)))
    '';
  drawPolicy = gridPolicy "draw-policy.lisp" false;
  blurPolicy = gridPolicy "blur-policy.lisp" true;
  blurGolden = "d2a3a62d11732227,04054e63f417ee38,9e63b3cd8690debe,193f78fdccd45f13,645f8e4b2bc7c567,cbb13ed365949372,63883fda131aaaba,02e7d909f5df0e72,afedfeedbb2f1a80";
  blurBackdrop = pkgs.writeText "blur-backdrop.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "draw-backdrop" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state
              (list (shell-surface :draw-backdrop (ui :row) :anchors '(:top :left)
                                   :margin '(24 0 0 24) :width 96 :height 48
                                   :layer :overlay :background "#00000000" :blur t))
              nil))
  '';
  timingPolicy = ./timing-policy.lisp;
  stallPolicy = pkgs.writeText "stall-policy.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "stall-clock" (:reads (:windows :button) :state '(:tick 0))
        (snapshot state event)
      (let* ((windows (context snapshot :windows))
             (tick (if (eq (getf event :type) :timer) (1+ (getf state :tick)) (getf state :tick))))
        (values (list :tick tick)
                (append
                 (list (interval :stall-clock 1000)
                       (shell-surface :stall-bar
                                      (bar-layout (list (ui :text :text (format nil "tick ~D" tick)))
                                                  (list (ui :text :text (clock-text "%H:%M:%S")))
                                                  (list (ui :text :text "stall")))
                                      :anchors '(:top :left :right) :height 24 :exclusive-zone 24
                                      :layer :overlay :background "#101010")
                       (shell-surface :stall-wallpaper (ui :stack)
                                      :anchors '(:top :right :bottom :left) :layer :background
                                      :background (list :shader "${../examples/shaders/aurora.glsl}"
                                                        :fps 30)))
                 (loop for window in windows for index from 0
                       collect (place (getf window :id) (* 180 index) 100
                                      (getf window :width) (getf window :height)))))))
  '';
  churnPolicy = pkgs.writeText "churn-policy.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "stall-churn" (:reads (:button) :state '(:ticks 0))
        (snapshot state event)
      (declare (ignore snapshot event))
      (let ((churn (make-list 60000 :initial-element (getf state :ticks))))
        (values (list :ticks (mod (length churn) 7))
                (list (interval :stall-churn 20))
                nil)))
  '';
  cSources = pkgs.lib.fileset.toSource {
    root = ./.;
    fileset = pkgs.lib.fileset.unions [
      (pkgs.lib.fileset.fileFilter (file: file.hasExt "c") ./.)
      ./harness.h
    ];
  };
  cTool =
    name: flags:
    pkgs.stdenv.mkDerivation {
      pname = "tomoe-test-${name}";
      version = "1";
      src = cSources;
      dontUnpack = true;
      strictDeps = true;
      buildPhase = ''
        $CC -std=gnu11 -O2 -Wall -Wextra -Werror -I$src $src/${name}.c -o tomoe-test-${name} ${flags}
      '';
      installPhase = "install -Dm755 tomoe-test-${name} $out/bin/tomoe-test-${name}";
    };
  cropTool = cTool "crop" "";
  ppmDiff = cTool "ppm-diff" "";
  memoryAttribute = cTool "memory-attribute" "";
  serviceProbe = cTool "service-probe" "";
  fakeSeatd = cTool "fake-seatd" "";
  glassMake = cTool "glass-make" "";
  captureCheck = cTool "capture-check" "";
  eventReader = cTool "event-reader" "";
  pointerProtocols = cTool "pointer-protocols" "";
  animationsTool = cTool "animations" "";
  shaderWait = cTool "shader-wait" "";
  cursorTool = cTool "cursor" "-pthread";
  liveCore = cTool "livecore" "-pthread";
  trailTool = pkgs.stdenv.mkDerivation {
    pname = "tomoe-test-trail";
    version = "1";
    src = cSources;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [ pkgs.pkg-config ];
    buildInputs = [ pkgs.zlib ];
    buildPhase = ''
      $CC -std=gnu11 -O2 -Wall -Wextra -Werror -I$src $src/trail.c -o tomoe-test-trail \
        $(pkg-config --cflags --libs zlib)
    '';
    installPhase = "install -Dm755 tomoe-test-trail $out/bin/tomoe-test-trail";
  };
  castClient = pkgs.stdenv.mkDerivation {
    pname = "tomoe-test-cast";
    version = "1";
    src = ./cast.c;
    dontUnpack = true;
    strictDeps = true;
    buildPhase = ''
      $CC -std=gnu11 -O2 -Wall -Wextra -Werror -I${../support} $src ${../support}/bus.c \
        -o tomoe-test-cast
    '';
    installPhase = "install -Dm755 tomoe-test-cast $out/bin/tomoe-test-cast";
  };
  frameclockPolicy = ./frameclock-policy.lisp;
  phasePolicy = ./phase-policy.lisp;
  phaseShader = "${tomoe}/share/tomoe/examples/shaders/aurora.glsl";
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
  dmabufClient = pkgs.stdenv.mkDerivation {
    name = "tomoe-dmabuf-client";
    src = ./dmabuf-client.c;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    buildInputs = [
      pkgs.wayland
      pkgs.libgbm
      pkgs.libdrm
    ];
    buildPhase = ''
      wp=${pkgs.wayland-protocols}/share/wayland-protocols
      for xml in $wp/stable/xdg-shell/xdg-shell.xml \
        $wp/stable/linux-dmabuf/linux-dmabuf-v1.xml \
        $wp/stable/viewporter/viewporter.xml; do
        name=$(basename "$xml" .xml)
        wayland-scanner client-header "$xml" "$name-client-protocol.h"
        wayland-scanner private-code "$xml" "$name-protocol.c"
      done
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src *-protocol.c \
        $(pkg-config --cflags --libs wayland-client gbm libdrm) -o tomoe-dmabuf-client
    '';
    installPhase = "install -Dm755 tomoe-dmabuf-client $out/bin/tomoe-dmabuf-client";
  };
  planesTool = pkgs.stdenv.mkDerivation {
    name = "tomoe-planes-probe";
    src = ./planes.c;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [ pkgs.pkg-config ];
    buildInputs = [ pkgs.libdrm ];
    buildPhase = ''
      $CC -std=c11 -O2 -Wall -Wextra -Werror $(pkg-config --cflags libdrm) $src \
        $(pkg-config --libs libdrm) -o tomoe-planes-probe
    '';
    installPhase = "install -Dm755 tomoe-planes-probe $out/bin/tomoe-planes-probe";
  };
  scanoutProbe = pkgs.writeText "scanout-probe.lisp" ''
    (in-package #:tomoe-user)

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
                                :background (list :trail :cell 9 :life 0.35
                                                  :steps 4 :color #xb8e0ffff :idle 0.5)))))))

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
  planesProbe = pkgs.writeText "planes-probe.lisp" ''
    (in-package #:tomoe-user)

    (defparameter +planes-states+ (list :note nil :moved nil :blur nil :chrome nil :rotate nil))

    (defun planes--toggle (state command)
      (let ((next (copy-list state)))
        (cond ((equal command "note-on") (setf (getf next :note) t))
              ((equal command "note-off") (setf (getf next :note) nil))
              ((equal command "blur-on") (setf (getf next :blur) t))
              ((equal command "blur-off") (setf (getf next :blur) nil))
              ((equal command "chrome-on") (setf (getf next :chrome) t))
              ((equal command "chrome-off") (setf (getf next :chrome) nil))
              ((equal command "rotate-ninety") (setf (getf next :rotate) :90))
              ((equal command "rotate-two-seventy") (setf (getf next :rotate) :270))
              ((equal command "rotate-off") (setf (getf next :rotate) nil))
              ((equal command "move") (setf (getf next :moved) t))
              ((equal command "unmove") (setf (getf next :moved) nil)))
        next))

    (defun planes--surfaces (state output)
      (let ((name (getf output :name)))
        (append
         (list (shell-surface :planes-wallpaper (ui :stack)
                              :output name :anchors '(:top :left :right :bottom)
                              :layer :background :background "#101418ff"))
         (when (getf state :chrome)
           (list (shell-surface :planes-bar
                                (ui :stack :children
                                    (list (ui :rect :width (getf output :width) :height 140
                                              :background "#ff8800ff")))
                                :output name :anchors '(:top :left :right)
                                :width (getf output :width) :height 140 :layer :top
                                :click-through t :background "#00000000")))
         (when (getf state :note)
           (list (shell-surface :planes-note
                                (ui :stack :children
                                    (list (ui :rect :width 200 :height 60 :background "#ffffffff")))
                                :output name :anchors '(:top :left) :margin '(120 0 0 200)
                                :width 200 :height 60 :layer :overlay
                                :background "#00000000")))
         (when (getf state :blur)
           (list (shell-surface :planes-frost (ui :row)
                                :output name :anchors '(:top :left) :margin '(120 0 0 200)
                                :width 200 :height 60 :layer :overlay
                                :background "#00000000" :blur t))))))

    (define-extension "planes-probe" (:reads (:command :windows :outputs) :state +planes-states+)
        (snapshot state event)
      (let* ((command (and (eq (getf event :type) :command)
                           (equal (getf event :owner) "planes-probe")
                           (getf event :name)))
             (next (if command (planes--toggle state command) state))
             (output (find "Virtual-1" (context snapshot :outputs)
                           :key (lambda (output) (getf output :name)) :test #'equal)))
        (values next
                (when output
                  (append (list (apply #'settings :animations nil
                                       (append (when (getf next :chrome)
                                                 (list :border (list :width 2 :radius 8
                                                                     :focused "#d0e0f0ff"
                                                                     :unfocused "#d0e0f0ff")))
                                               (when (getf next :blur)
                                                 (list :blur (list :enabled t :passes 2 :offset 2)))))
                                (define-command "note-on")
                                (define-command "note-off")
                                (define-command "blur-on")
                                (define-command "blur-off")
                                (define-command "chrome-on")
                                (define-command "chrome-off")
                                (define-command "rotate-ninety")
                                (define-command "rotate-two-seventy")
                                (define-command "rotate-off")
                                (define-command "move")
                                (define-command "unmove"))
                          (when (getf next :rotate)
                            (list (configure-output (getf output :name)
                                                    :transform (getf next :rotate))))
                          (loop for window in (context snapshot :windows)
                                for id = (getf window :id)
                                for app = (getf window :app-id)
                                collect (if (or (equal app "scanout-client")
                                                (equal app "dmabuf-check"))
                                            (if (getf next :moved)
                                                (place id (floor (getf output :width) 4)
                                                       (floor (getf output :height) 4)
                                                       (floor (getf output :width) 2)
                                                       (floor (getf output :height) 2))
                                                (place id (floor (getf output :width) 8)
                                                       (floor (getf output :height) 8)
                                                       (floor (getf output :width) 2)
                                                       (floor (getf output :height) 2)))
                                            (place id 100 100 200 150)))
                          (planes--surfaces next output)))
                nil)))
  '';
  portalConsumer = pkgs.stdenv.mkDerivation {
    name = "tomoe-portal-consumer";
    src = ./portal-consumer.c;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [ pkgs.pkg-config ];
    buildInputs = [ pkgs.pipewire ];
    buildPhase = ''
      $CC -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Wno-unused-parameter $src \
        $(pkg-config --cflags --libs libpipewire-0.3) -o tomoe-portal-consumer
    '';
    installPhase = "install -Dm755 tomoe-portal-consumer $out/bin/tomoe-portal-consumer";
  };
  pathClient = pkgs.stdenv.mkDerivation {
    name = "tomoe-test-pointer-path";
    source = ./pointer-path.c;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    buildInputs = [ pkgs.wayland ];
    buildPhase = ''
      wayland-scanner client-header ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml wlr-virtual-pointer-unstable-v1-client-protocol.h
      wayland-scanner private-code ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml wlr-virtual-pointer-unstable-v1-protocol.c
      gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -I. "$source" *.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-test-pointer-path
    '';
    installPhase = "install -Dm755 tomoe-test-pointer-path $out/bin/tomoe-test-pointer-path";
  };
  memorySound = pkgs.runCommand "tomoe-memory-sound" { } ''
    mkdir -p $out
    {
      printf 'RIFF'
      printf '\244\045\000\000'
      printf 'WAVEfmt '
      printf '\020\000\000\000\001\000\001\000'
      printf '\200\273\000\000\000\167\001\000'
      printf '\002\000\020\000'
      printf 'data'
      printf '\200\045\000\000'
      head -c 9600 /dev/zero
    } > $out/sound.wav
  '';
  memoryConfig = pkgs.runCommand "tomoe-memory-config" { } ''
    mkdir -p $out/shaders
    install -m 644 ${./memory-config.lisp} $out/init.lisp
    install -m 644 ${./memory-gtk.css} $out/gtk.css
    install -m 644 ${memorySound}/sound.wav $out/sound.wav
    for name in aurora cubes liquify okazz-glyphs okazz-spheres stars towers xmb; do
      install -m 644 ${tomoe}/share/tomoe/examples/shaders/$name.glsl $out/shaders/$name.glsl
    done
    for name in balatro cassette-led cassette-squares dot-corridor glass-mound hex-spiral llama-herd llama-herd-night su-ei; do
      install -m 644 ${tomoe}/share/tomoe/examples/shaders/okazz-glyphs.glsl $out/shaders/$name.glsl
    done
  '';
  benchClient = pkgs.callPackage ../bench/client.nix { };
  vm =
    {
      name,
      suites,
      packages ? [ ],
      kernelModules ? [
        "vgem"
        "udmabuf"
      ],
      extraModprobeConfig ? "",
      timeout ? 3600,
      memory ? 2048,
      qemuOptions ? [ ],
      uinput ? false,
    }:
    pkgs.testers.runNixOSTest {
      inherit name;
      globalTimeout = timeout;
      nodes.machine = {
        boot.kernelModules = kernelModules;
        boot.extraModprobeConfig = extraModprobeConfig;
        hardware.graphics.enable = true;
        hardware.uinput.enable = uinput;
        services.seatd.enable = true;
        systemd.services.seatd.environment.SEATD_VTBOUND = "0";
        environment.systemPackages = [
          tomoe
          client
          imeClient
          pkgs.grim
          pkgs.libnotify
          pkgs.xdpyinfo
          pkgs.wayland-utils
        ]
        ++ packages;
        environment.pathsToLink = [
          "/share/dbus-1"
          "/share/xdg-desktop-portal"
        ];
        environment.variables.LP_NUM_THREADS = "4";
        virtualisation.memorySize = memory;
        virtualisation.cores = 4;
        virtualisation.qemu.options = qemuOptions;
      };
      testScript = builtins.readFile ./tomoe.py + ''
        import json
        import traceback

        with open("${pkgs.writeText "${name}-suites.json" (builtins.toJSON suites)}") as file:
            suites = json.load(file)

        def services():
            listing = machine.execute(
                "systemctl list-units --all --type=service --plain --no-legend",
                check_return=False)[1]
            return sorted({line.split()[0] for line in listing.splitlines() if line.split()})

        def transients():
            found = []
            for unit in services():
                path = machine.execute("systemctl show -P FragmentPath " + unit,
                                       check_return=False)[1].strip()
                if path.startswith("/run/systemd/transient/"):
                    found.append(unit)
            return found

        def reset():
            for unit in transients():
                machine.execute("systemctl stop " + unit, check_return=False)
                machine.execute("systemctl reset-failed " + unit, check_return=False)
            left = transients()
            if left:
                print("tomoe reset could not clear: " + " ".join(left), flush=True)
            machine.execute("rm -rf /run/tomoe /run/check-bus /run/bus-probe "
                            "/run/security-globals /run/fifo-globals /run/trail /run/cursor "
                            "/run/animations-frames /run/memory /run/trace.log "
                            "/run/layout.log /run/trace-swap.lisp /run/draw.ppm "
                            "/run/tile.ppm /run/timing.ppm /run/*-display.ppm "
                            "/run/*-shot.ppm /tmp/.X11-unix/* "
                            "/tmp/.X*-lock /run/core.*", check_return=False)

        def backtrace():
            found = machine.execute("ls /run/core.* 2>/dev/null",
                                    check_return=False)[1].split()
            if not found:
                return
            print("tomoe core: " + " ".join(found), flush=True)
            if machine.execute("command -v gdb")[0] != 0:
                print("tomoe core: no gdb in this test", flush=True)
                return
            frames = machine.execute(
                "gdb -batch -ex 'thread apply all bt 12' $(readlink -f $(command -v tomoe)) "
                + found[-1] + " 2>&1",
                check_return=False)[1]
            print("tomoe core backtrace:\n" + "\n".join(frames.splitlines()[-120:]),
                  flush=True)

        failures = []
        machine.execute("sysctl -w kernel.core_pattern=/run/core.%p", check_return=False)
        for suite in suites:
            print("tomoe suite: " + suite["name"], flush=True)
            reset()
            environment = dict(globals())
            try:
                exec(compile(suite["script"], "<" + suite["name"] + ">", "exec"), environment)
            except Exception:
                failures.append((suite["name"], traceback.format_exc()))
                print("tomoe suite FAILED: " + suite["name"] + "\n"
                      + failures[-1][1], flush=True)
                backtrace()
            else:
                print("tomoe suite passed: " + suite["name"], flush=True)
        reset()
        if failures:
            raise AssertionError(
                str(len(failures)) + " of " + str(len(suites)) + " suites failed: "
                + ", ".join(name for name, _ in failures) + "\n"
                + "\n".join("--- " + name + " ---\n" + trace for name, trace in failures))
      '';
    };
  animationUnit = pkgs.stdenv.mkDerivation {
    name = "tomoe-animation-unit";
    src = ./..;
    strictDeps = true;
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    buildInputs = [
      pkgs.wayland
      pkgs.libdrm
      pkgs.libGL
    ];
    dontConfigure = true;
    buildPhase = ''
      wayland-scanner -c server-header ${pkgs.wayland-scanner}/share/wayland/wayland.xml \
        wayland-server-protocol.h
      wayland-scanner -c client-header ${pkgs.wayland-scanner}/share/wayland/wayland.xml \
        wayland-client-protocol.h
      wayland-scanner client-header \
        ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
        wlr-layer-shell-unstable-v1-protocol.h
      wayland-scanner client-header \
        ${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
        xdg-shell-protocol.h
      $CC -std=c11 -D_GNU_SOURCE -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
        -Inative -Ilib/lisp/include -Ilib/xkb/include -Ilib/wayland/include -Ilib/wayland/scanner -I. \
        tests/animation-unit.c native/animation.c \
        $(pkg-config --cflags libdrm glesv2) \
        $(pkg-config --libs wayland-server libdrm glesv2) -lm \
        -o animation-unit
      ./animation-unit
    '';
    installPhase = "install -Dm755 animation-unit $out/bin/tomoe-animation-unit";
  };
  hostUnit =
    name: target: run:
    tomoe.overrideAttrs (old: {
      pname = "tomoe-${name}";
      src = ./..;
      buildPhase = ''
        runHook preBuild
        make -j"$NIX_BUILD_CORES" build/${target} \
          WLR=${pkgs.wlr-protocols}/share/wlr-protocols/unstable \
          WP=${pkgs.wayland-protocols}/share/wayland-protocols \
          WAYLAND_XML=${pkgs.wayland-scanner}/share/wayland/wayland.xml
        ${run}
        runHook postBuild
      '';
      installPhase = "install -Dm755 build/${target} $out/bin/tomoe-${name}";
    });
  lispUnit = hostUnit "lisp-unit" "lisp-unit" "make lisp-unit";
  eventRing = hostUnit "event-ring" "event-ring" "build/event-ring";
  portalFormat = hostUnit "portal-format" "portal-format" "build/portal-format";
  xkbEquivalence = pkgs.stdenv.mkDerivation {
    name = "tomoe-xkb-equivalence";
    src = pkgs.lib.fileset.toSource {
      root = ./..;
      fileset = pkgs.lib.fileset.unions [
        ../lib/xkb
        ./xkb-trace.c
        ./xkb-cases.awk
      ];
    };
    strictDeps = true;
    nativeBuildInputs = [ pkgs.pkg-config ];
    buildInputs = [ pkgs.libxkbcommon ];
    dontConfigure = true;
    buildPhase = ''
      root=${pkgs.xkeyboard_config}/etc/X11/xkb
      build() {
        $CC -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Ilib/xkb/include \
          -DTOMOE_XKB_CONFIG_ROOT="\"$root\"" tests/xkb-trace.c lib/xkb/*.c "$@"
      }
      build $(pkg-config --cflags --libs xkbcommon) -o xkb-trace
      build -DNATIVE -o xkb-trace-native
      trace() {
        if [ "$1" = native ]; then ./xkb-trace-native "''${@:2}"; else ./xkb-trace "$@"; fi
      }
      awk -f tests/xkb-cases.awk $root/rules/evdev.xml $root/rules/evdev.extras.xml > cases
      split -n r/$NIX_BUILD_CORES -d -a 3 cases part.
      for part in part.*; do
        for mode in real native client text; do
          trace $mode < $part > $part.$mode &
        done
      done
      wait
      for mode in real native client text; do
        cat part.*.$mode | sort > $mode.trace
        if [ "$(wc -l < $mode.trace)" != "$(wc -l < cases)" ]; then
          echo "xkb-trace $mode stopped before the last case"
          exit 1
        fi
      done
      status=0
      for pair in real:native client:text; do
        want=''${pair%:*} got=''${pair#*:}
        if diff $want.trace $got.trace > $got.diff; then
          echo "$got matches $want on $(wc -l < cases) cases"
          continue
        fi
        status=1
        echo "$got differs from $want on $(grep -c '^>' $got.diff) of $(wc -l < cases) cases:"
        grep '^>' $got.diff | cut -c3- | rev | cut -f3- | rev | head -20
        grep -m1 '^>' $got.diff | cut -c3- | rev | cut -f3- | rev > first
        trace $want full < first > $want.full
        trace $got full < first > $got.full
        diff $want.full $got.full | head -40 || true
      done
      [ $status = 0 ]
    '';
    installPhase = ''
      mkdir -p $out/share/tomoe
      cat real.trace > $out/share/tomoe/xkb-equivalence
    '';
  };
  libraries = pkgs.stdenv.mkDerivation {
    name = "tomoe-libraries";
    src = ../lib;
    strictDeps = true;
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    dontConfigure = true;
    buildPhase = ''
      for library in png lisp wayland xkb pipewire; do
        make -C $library -j"$NIX_BUILD_CORES" \
          WAYLAND_XML=${pkgs.wayland-scanner}/share/wayland/wayland.xml
      done
    '';
    installPhase = ''
      for library in png lisp wayland xkb pipewire; do
        install -Dm644 -t $out/lib $library/build/libtomoe-$library.a $library/build/libtomoe-$library.so
        install -Dm644 -t $out/include/tomoe-$library $library/include/*.h
      done
      install -Dm644 -t $out/include/tomoe-wayland/scanner wayland/scanner/*.h wayland/build/wayland-*-protocol.h
    '';
  };
  compositorPackages = [
    libraries
    animationUnit
    xkbEquivalence
    lispUnit
    eventRing
    portalFormat
    pkgs.gdb
    pkgs.wayland-utils
    pkgs.pipewire
    pkgs.xdg-desktop-portal
    pkgs.wlr-randr
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
    securityClient
    outputClient
    x11Client
    scanoutClient
    pathClient
  ];
  compositorSuites = [
    {
      name = "livecore";
      script = ''
        machine.wait_for_unit("multi-user.target")
        machine.succeed("MESA_LOADER_DRIVER_OVERRIDE=zink "
          + "${liveCore}/bin/tomoe-test-livecore "
          + "${tomoe}/bin/tomoe ${tomoe}/lib/tomoe/core.so "
          + "${coreB}/lib/tomoe/core.so ${coreBad}/lib/tomoe/core.so "
          + "${client}/bin/tomoe-test-client ${client}/bin/tomoe-pointer-script "
          + "${client}/bin/tomoe-test-pointer-client ${./livecore-policy.lisp} core-b")
      '';
    }
    {
      name = "pointer-protocols";
      script = ''
        machine.wait_for_unit("multi-user.target")
        machine.succeed("MESA_LOADER_DRIVER_OVERRIDE=zink "
          + "${pointerProtocols}/bin/tomoe-test-pointer-protocols "
          + "${tomoe}/bin/tomoe ${client}/bin/tomoe-test-pointer-client")
      '';
    }
    {
      name = "security";
      script = ''
        start("--bare", bus=False)
        machine.succeed("env -i HOME=/root XDG_RUNTIME_DIR=/run/tomoe WAYLAND_DISPLAY=check ${pkgs.wayland-utils}/bin/wayland-info > /run/security-globals")
        machine.succeed("env -i HOME=/root XDG_RUNTIME_DIR=/run/tomoe WAYLAND_DISPLAY=check ${securityClient}/bin/tomoe-security-client")
        quit()
      '';
    }
    {
      name = "text-input";
      script = ''
        start("--bare")
        cli("mount ${imePolicy}")
        machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-ime-client text-only")
        quit()
      '';
    }
    {
      name = "input-method";
      script = ''
        start("--bare")
        cli("mount ${imePolicy}")
        machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-ime-client")
        quit()
      '';
    }
    {
      name = "shortcuts-inhibit";
      script = ''
        start("--bare")
        cli("mount ${imePolicy}")
        cli("mount ${shortcutBindings}")
        machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-ime-client shortcuts")
        quit()
      '';
    }
    {
      name = "surface-protocols";
      script = ''
        SIZE_PROBE = "${sizeProbe}"
      ''
      + builtins.readFile ./surface-protocols.py;
    }
    {
      name = "integration";
      script = ''
        ZOOMER = "${tomoe}/share/tomoe/examples/zoomer.lisp"
        SCREENSHOT_PROBE = "${screenshotProbe}"
        SCREEN_CLIPBOARD_PROBE = "${screenClipboardProbe}"
        OFFSCREEN_PROBE = "${offscreenProbe}"
        BLUR_PROBE = "${blurProbe}"
        THEME_PROBE = "${themeProbe}"
        CLOCK_PROBE = "${clockProbe}"
        VIEWPORT_PROBE = "${viewportProbe}"
      ''
      + builtins.readFile ./integration.py;
    }
    {
      name = "commands";
      script = ''
        POWER_POLICY = "${./power-policy.lisp}"
      ''
      + builtins.readFile ./commands.py;
    }
    {
      name = "nofocus";
      script = ''
        DECK = "${tomoe}/share/tomoe/examples/deck.lisp"
        BUILTIN = "${tomoe}/share/tomoe/lisp/desktop.lisp"
        PROBE = "${./nofocus-probe.lisp}"
      ''
      + builtins.readFile ./nofocus.py;
    }
    {
      name = "bare";
      script = ''
        VIRTUAL_PROBE = "${virtualProbe}"
      ''
      + builtins.readFile ./bare.py;
    }
    {
      name = "fifo";
      script = builtins.readFile ./fifo.py;
    }
    {
      name = "xwayland";
      script = ''
        X11_CLIENT = "${x11Client}/bin/tomoe-x11-client"
        X11_FONTS = "${pkgs.fontmiscmisc}/share/fonts/X11/misc"
      ''
      + builtins.readFile ./xwayland.py;
    }
    {
      name = "output-management";
      script = ''
        VIRTUAL_PROBE = "${virtualProbe}"
        OUTPUT_OVERRIDE_PROBE = "${outputOverrideProbe}"
      ''
      + builtins.readFile ./output-management.py;
    }
    {
      name = "portal";
      script = ''
        CAST = "${castClient}/bin/tomoe-test-cast"
        PIPEWIRE_CONFIG = "${pkgs.pipewire}/share/pipewire"
        CONSUMER = "${portalConsumer}/bin/tomoe-portal-consumer"
        POINTER = "${pathClient}/bin/tomoe-test-pointer-path"
        PORTAL_SUITE = "${./portal-suite.py}"
      ''
      + builtins.readFile ./portal.py;
    }
    {
      name = "session-bus";
      script = ''
        BUS_PROBE = "${busProbe}"
      ''
      + builtins.readFile ./session-bus.py;
    }
    {
      name = "clean-unmount";
      script = ''
        WALLPAPER_SETTINGS = "${wallpaperSettings}"
        CLEAN_BASELINE = "${cleanBaseline}"
        SOURCES = ["${tomoe}/share/tomoe/lisp/desktop.lisp"] + sorted(
            "${tomoe}/share/tomoe/examples/" + name for name in ${builtins.toJSON (builtins.filter (pkgs.lib.hasSuffix ".lisp") (builtins.attrNames (builtins.readDir ../examples)))}
        )
      ''
      + builtins.readFile ./clean-unmount.py;
    }
    {
      name = "livepatch";
      script = ''
        GAP_PATCH = "${livePatchGap}"
        KEYBOARD_PATCH = "${livePatchKeyboard}"
        SLOW_SOURCE = "${livePatchSlow}"
        FLOOD_SOURCE = "${livePatchFlood}"
        CANVAS_SOURCE = "${livePatchCanvas}"
      ''
      + builtins.readFile ./livepatch.py;
    }
    {
      name = "draw";
      script = ''
        DRAW_POLICY = "${drawPolicy}"
        DRAW_COUNTS = "1,10,50"
        DRAW_GOLDEN = "${drawGolden}"
        BLUR_POLICY = "${blurPolicy}"
        BLUR_BACKDROP = "${blurBackdrop}"
        BLUR_GOLDEN = "${blurGolden}"
      ''
      + builtins.readFile ./draw.py;
    }
    {
      name = "overdraw";
      script = ''
        OVERDRAW_COUNTS = "1,10,50"
        OVERDRAW_GOLDEN = "${overdrawGolden}"
        OVERDRAW_BASELINE = ""
        OVERDRAW_WALLPAPER = "${./overdraw-wallpaper.glsl}"
      ''
      + builtins.readFile ./overdraw.py;
    }
    {
      name = "ui-damage";
      script = ''
        DAMAGE_POLICY = "${./ui-damage-policy.lisp}"
      ''
      + builtins.readFile ./ui-damage.py;
    }
    {
      name = "timing";
      script = ''
        TIMING_POLICY = "${timingPolicy}"
        TIMING_CONTRACT_POLICY = "${./timing-contract-policy.lisp}"
        STALL_POLICY = "${stallPolicy}"
        CHURN_POLICY = "${churnPolicy}"
        RUNS = "3"
        COUNT = "200"
      ''
      + builtins.readFile ./timing.py;
    }
    {
      name = "trace";
      script = ''
        TRACE_CHAIN = "${./trace-chain.lisp}"
        TRACE_RULES = "${./trace-rules.lisp}"
        TRACE_FAIL = "${./trace-fail.lisp}"
        TRACE_CYCLE = "${./trace-cycle.lisp}"
        TRACE_MOUNT_A = "${./trace-mount-a.lisp}"
        TRACE_MOUNT_B = "${./trace-mount-b.lisp}"
        TRACE_GRID = "${./trace-grid.lisp}"
        TRACE_DELAY = "${./trace-delay.lisp}"
        TRACE_SWAP = "${./trace-swap.lisp}"
        TRACE_GOLDEN = ${if builtins.pathExists ./trace.golden then "\"${./trace.golden}\"" else "\"\""}
      ''
      + builtins.readFile ./trace.py;
    }
    {
      name = "animations";
      script = ''
        machine.succeed("mkdir -p /run/animations-frames")
        machine.succeed(
          "ANIMATIONS_REPORT=/run/animations-frames/report.txt "
          + "${animationsTool}/bin/tomoe-test-animations --tomoe ${tomoe}/bin/tomoe"
          + " --client ${client}/bin/tomoe-test-client --frames /run/animations-frames"
        )
        print(machine.succeed("cat /run/animations-frames/report.txt"))
      '';
    }
    {
      name = "trail";
      script = ''
        machine.succeed("mkdir -p /run/trail")
        print(machine.succeed("LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=zink "
          + "${trailTool}/bin/tomoe-test-trail --tomoe ${tomoe}/bin/tomoe"
          + " --pointer ${pathClient}/bin/tomoe-test-pointer-path --trail --work /run/trail"))
      '';
    }
    {
      name = "shader-wait";
      script = ''
        machine.succeed("mkdir -p /run/shader-wait")
        print(machine.succeed(
          "timeout 300 ${shaderWait}/bin/tomoe-test-shader-wait --tomoe ${tomoe}/bin/tomoe"
          + " --work /run/shader-wait"))
      '';
    }
    {
      name = "cursor";
      script = ''
        machine.succeed("mkdir -p /run/cursor")
        status, output = machine.execute(
          "LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=zink "
          + "${cursorTool}/bin/tomoe-test-cursor --tomoe ${tomoe}/bin/tomoe "
          + "--pointer ${pathClient}/bin/tomoe-test-pointer-path --work /run/cursor 2>&1")
        print(output)
        if status != 0:
            raise AssertionError("the cursor proof failed: see the report above")
      '';
    }
    {
      name = "scanout";
      script = ''
        SCANOUT_CLIENT = "${scanoutClient}/bin/tomoe-scanout-client"
        SCANOUT_PROBE = "${scanoutProbe}"
      ''
      + builtins.readFile ./scanout.py;
    }
    {
      name = "window-capture";
      script = ''
        CAPTURE_CLIENT = "${client}/bin/tomoe-capture-client"
        CAPTURE_POLICY = "${capturePolicy}"
        CAPTURE_CHECK = "${captureCheck}/bin/tomoe-test-capture-check"
      ''
      + builtins.readFile ./window-capture.py;
    }
    {
      name = "input-stream";
      script = ''
        SCRIPT = "${client}/bin/tomoe-pointer-script"
        INPUT_POLICY = "${inputPolicy}"
        SHORTCUT_POLICY = "${shortcutBindings}"
        EVENT_READER = "${eventReader}/bin/tomoe-test-event-reader"
      ''
      + builtins.readFile ./input-stream.py;
    }
    {
      name = "input-lock";
      script = ''
        SCRIPT = "${client}/bin/tomoe-pointer-script"
        LOCK_CLIENT = "${client}/bin/tomoe-lock-client"
        EVENT_READER = "${eventReader}/bin/tomoe-test-event-reader"
      ''
      + builtins.readFile ./input-lock.py;
    }
    {
      name = "memory";
      script = ''
        CONFIG = "/run/memory/init.lisp"
        SOURCE = "${memoryConfig}"
        TOMOE_BIN = "${tomoe}/bin/tomoe"
        ATTRIBUTE = "${memoryAttribute}/bin/tomoe-test-memory-attribute"
        SCENE_POLICY = "${./timing-policy.lisp}"
        REFRESH_POLICY = "${./memory-refresh.lisp}"
        HANDBACK_POLICY = "${./memory-handback.lisp}"
        HANDBACK_GATE = "${./memory-handback.py}"
      ''
      + builtins.readFile ./memory.py;
    }
  ];
  equivalenceSuites = [
    {
      name = "input-equivalence";
      script = ''
        FASTPATH = "${tomoe}/bin/tomoe"
        INJECTOR = "${scenario}/bin/tomoe-scenario"
        EQUIVALENCE_DESKTOP = "${./equivalence-desktop.txt}"
        EQUIVALENCE_COUNTER = "${./equivalence-counter.txt}"
        POLICY = "${scenarioPolicy}"
        COUNTER_POLICY = "${counterPolicy}"
        CONTRACT_POLICY = "${contractPolicy}"
      ''
      + builtins.readFile ./equivalence.py;
    }
  ];
  kmsSettings = {
    kernelModules = [
      "vgem"
      "udmabuf"
      "vkms"
    ];
    extraModprobeConfig = "options vkms enable_overlay=1 enable_cursor=1";
    packages = [
      scanoutClient
      dmabufClient
      planesTool
    ];
    timeout = 1800;
  };
  kmsSuites = [
    {
      name = "seat";
      script = ''
        FAKE_SEATD = "${fakeSeatd}/bin/tomoe-test-fake-seatd"
      ''
      + builtins.readFile ./seat.py;
    }
    {
      name = "kms-wake";
      script = ''
        WAKE_PROBE = "${wakeProbe}"
      ''
      + builtins.readFile ./kms-wake.py;
    }
    {
      name = "frameclock";
      script = ''
        FRAMECLOCK_POLICY = "${frameclockPolicy}"
      ''
      + builtins.readFile ./frameclock.py;
    }
    {
      name = "phase";
      script = ''
        PHASE_POLICY = "${phasePolicy}"
        PHASE_SHADER = "${phaseShader}"
        RUNS = "3"
      ''
      + builtins.readFile ./phase.py;
    }
    {
      name = "glass";
      script = ''
        SCANOUT_CLIENT = "${scanoutClient}/bin/tomoe-scanout-client"
        GLASS_MAKE = "${glassMake}/bin/tomoe-test-glass-make"
      ''
      + builtins.readFile ./glass.py;
    }
    {
      name = "planes";
      script = ''
        SCANOUT_CLIENT = "${scanoutClient}/bin/tomoe-scanout-client"
        DMABUF_CLIENT = "${dmabufClient}/bin/tomoe-dmabuf-client"
        PLANES_PROBE = "${planesProbe}"
        PPM_CROP = "${cropTool}/bin/tomoe-test-crop"
        PPM_DIFF = "${ppmDiff}/bin/tomoe-test-ppm-diff"
      ''
      + builtins.readFile ./planes.py;
    }
    {
      name = "session";
      script = ''
        FRAMECLOCK_POLICY = "${frameclockPolicy}"
      ''
      + builtins.readFile ./session.py;
    }
  ];
  suiteTargets =
    settings: suites:
    builtins.listToAttrs (
      map (suite: {
        name = suite.name;
        value = vm (
          settings
          // {
            name = "tomoe-${suite.name}";
            suites = [ suite ];
          }
        );
      }) suites
    );
  compositorSettings = {
    packages = compositorPackages;
  };
  smokeSuites = builtins.filter (
    suite:
    builtins.elem suite.name [
      "integration"
      "pointer-protocols"
      "livepatch"
      "livecore"
    ]
  ) compositorSuites;
  smokeSettings = compositorSettings // {
    timeout = 900;
  };
  servicesSuite = {
    name = "services";
    script = ''
      SERVICE_PROBE = "${serviceProbe}/bin/tomoe-test-service-probe"
    '' + builtins.readFile ./services.py;
  };
  servicesCheck = vm (
    compositorSettings
    // {
      name = "tomoe-services";
      timeout = 900;
      suites = [ servicesSuite ];
    }
  );
  equivalenceSettings = compositorSettings // {
    timeout = 1800;
  };
  benchClosure = package: pkgs.closureInfo { rootPaths = [ package ]; };
  benchNiri = pkgs.callPackage ../bench/niri.nix { };
  benchScript =
    compositors:
    ''
      BENCH = "${./../bench}"
      PATHS = {
          "client": "${benchClient}/bin/bench-client",
          "compositors": "${compositors}",
          "cores": "4",
          "memory": "4096",
          "closures": {
              "tomoe": "${benchClosure tomoe}",
              "sway": "${benchClosure pkgs.sway}",
              "niri": "${benchClosure benchNiri}",
              "hyprland": "${benchClosure pkgs.hyprland}",
          },
      }
      exec(compile(open(BENCH + "/driver.py").read()
                   + "\nmain(machine, PATHS)\n", "bench", "exec"))
    '';
  benchSettings = {
    kernelModules = [
      "vkms"
      "vgem"
      "udmabuf"
    ];
    packages = [
      benchClient
      pkgs.sway
      benchNiri
      pkgs.hyprland
      pkgs.procps
    ];
    qemuOptions = [
      "-vga"
      "none"
    ];
    uinput = true;
    memory = 4096;
    timeout = 3600;
  };
  benchSuites = [
    {
      name = "bench";
      script = benchScript "tomoe,sway,niri,hyprland";
    }
    {
      name = "bench-competitors";
      script = benchScript "sway,hyprland,niri";
    }
    {
      name = "bench-tomoe";
      script = benchScript "tomoe";
    }
  ];
in
{
  inherit client;
  inherit libraries;
  lisp-unit = lispUnit;
  event-ring = eventRing;
  xkb-equivalence = xkbEquivalence;
  lease = pkgs.runCommand "tomoe-lease" {
    nativeBuildInputs = [
      pkgs.bash
      pkgs.util-linux
      pkgs.coreutils
    ];
    LEASE_LIB = "${../tools/lease.sh}";
  } ''
    bash ${./lease.sh}
    touch $out
  '';
  compositor = vm (
    compositorSettings
    // {
      name = "tomoe-compositor";
      suites = compositorSuites;
    }
  );
  equivalence = vm (
    equivalenceSettings
    // {
      name = "tomoe-equivalence";
      suites = equivalenceSuites;
    }
  );
  kms = vm (
    kmsSettings
    // {
      name = "tomoe-kms";
      suites = kmsSuites;
    }
  );
  smoke = vm (
    smokeSettings
    // {
      name = "tomoe-smoke";
      suites = smokeSuites;
    }
  );
  services = servicesCheck;
  suites =
    suiteTargets compositorSettings compositorSuites
    // suiteTargets equivalenceSettings equivalenceSuites
    // suiteTargets kmsSettings kmsSuites
    // { services = servicesCheck; }
    // suiteTargets benchSettings benchSuites;
}
