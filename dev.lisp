(require :sb-posix)
(require :sb-bsd-sockets)

(defun argument-value (option)
  (loop for (name value) on (rest sb-ext:*posix-argv*)
        when (equal name option) return value))

(setf sb-ext:*posix-argv*
      (cons (first sb-ext:*posix-argv*)
            (cdr (member "--" sb-ext:*posix-argv* :test #'equal))))

(dolist (name '("api" "json" "patterns" "ui" "runtime" "rules" "timers" "watches" "executions" "processes" "control" "ipc-transport" "ipc" "notifications" "mpris" "battery" "network" "tray" "main"))
  (load (format nil "src/~A.lisp" name) :verbose nil :print nil))

(if (equal (argument-value "--backend") "lisp")
    (dolist (file '("backend/core-protocols.lisp" "backend/xdg-shell.lisp"
                    "backend/wl.lisp" "backend/server.lisp"))
      (load file :verbose nil :print nil))
    (load "src/native.lisp" :verbose nil :print nil))

(sb-ext:exit :code (handler-case (tomoe::main)
                     (serious-condition (condition)
                       (format *error-output* "tomoe: ~A~%" condition)
                       1)))
