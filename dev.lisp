(require :sb-posix)
(require :sb-bsd-sockets)

(setf sb-ext:*posix-argv*
      (cons (first sb-ext:*posix-argv*)
            (cdr (member "--" sb-ext:*posix-argv* :test #'equal))))

(dolist (name '("api" "json" "patterns" "ui" "runtime" "rules" "timers" "watches" "executions" "processes" "control" "ipc-transport" "ipc" "notifications" "mpris" "battery" "network" "tray" "main"))
  (load (format nil "src/~A.lisp" name) :verbose nil :print nil))

(load "src/native.lisp" :verbose nil :print nil)

(sb-ext:exit :code (handler-case (tomoe::main)
                     (serious-condition (condition)
                       (format *error-output* "tomoe: ~A~%" condition)
                       1)))
