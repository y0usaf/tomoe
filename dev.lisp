;;; Run the compositor from the working tree. No Nix build, no saved image.
;;; `sbcl --load dev.lisp -- --backend lisp` selects the Lisp backend;
;;; any other backend uses the wlroots shim, which must be compiled first.
(require :sb-posix)
(require :sb-bsd-sockets)

(defun argument-value (option)
  (loop for (name value) on (rest sb-ext:*posix-argv*)
        when (equal name option) return value))

;; SBCL keeps the "--" marker in *posix-argv*; main reads that list directly.
(setf sb-ext:*posix-argv*
      (cons (first sb-ext:*posix-argv*)
            (cdr (member "--" sb-ext:*posix-argv* :test #'equal))))

(dolist (name '("api" "runtime" "control" "main"))
  (load (format nil "src/~A.lisp" name) :verbose nil :print nil))

;; Exactly one backend is loaded, so the two definitions never coexist.
(if (equal (argument-value "--backend") "lisp")
    (dolist (file '("backend/core-protocols.lisp" "backend/xdg-shell.lisp"
                    "backend/wl.lisp" "backend/server.lisp"))
      (load file :verbose nil :print nil))
    (load "src/native.lisp" :verbose nil :print nil))

(sb-ext:exit :code (handler-case (tomoe::main)
                     (serious-condition (condition)
                       (format *error-output* "tomoe: ~A~%" condition)
                       1)))
