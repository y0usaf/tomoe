(defvar *definitions* :not-loading)
(defvar *source*)
(defvar *source-directory*)
(defvar *copy-data-nodes* 32768)
(defvar *copy-data-chars* 65536)

(defun %finite-float-p (value)
  "True when VALUE is an IEEE float with a finite value."
  (and (floatp value)
       (= value value)
       (<= (abs value) 1.7976931348623157d308)))

(defun %finite-real-p (value)
  (and (realp value)
       (or (not (floatp value)) (%finite-float-p value))))

(defun %double-float (value)
  "Coerce a finite real to a finite double-float, or signal a type error."
  (unless (%finite-real-p value)
    (error "Expected a finite real number, got ~S." value))
  (let ((result (ignore-errors (coerce value 'double-float))))
    (unless (and result (%finite-float-p result))
      (error "Real number cannot be represented as a finite double-float: ~S."
             value))
    result))

(defun %round-away-positive (value)
  "Round a non-negative REAL away from zero at an exact half."
  (floor (+ (coerce value 'double-float) 0.5d0)))

(defun copy-data (value)
  "Copy bounded Lisp data. No closures, host handles, shared strings, or cycles cross dispatch."
  (let ((nodes 0))
    (labels ((walk (item depth)
               (when (or (> (incf nodes) *copy-data-nodes*) (> depth 64))
                 (error "Extension data exceeds the node/depth budget."))
               (cond
                 ((null item) nil)
                 ((consp item)
                  (let ((items nil) (tail item))
                    (loop while (consp tail) do
                      (when (> (incf nodes) *copy-data-nodes*)
                        (error "Extension data exceeds the node budget."))
                      (push (walk (car tail) (1+ depth)) items)
                      (setf tail (cdr tail)))
                    (append (nreverse items) (walk tail (1+ depth)))))
                 ((stringp item)
                  (when (> (length item) *copy-data-chars*)
                    (error "String exceeds ~D characters." *copy-data-chars*))
                  (copy-seq item))
                 ((or (integerp item) (rationalp item)) item)
                 ((floatp item)
                  (unless (%finite-float-p item)
                    (error "Only finite floats are extension data."))
                  item)
                 ((symbolp item) item)
                 (t (error "Not extension data: ~S" item)))))
      (walk value 0))))

(defparameter %nul (code-char 0))

(defun %same-elements-p (first second)
  (and (= (length first) (length second))
       (loop for left in first for right in second always (eq left right))))

(defun %boolean-p (value) (or (eq value t) (null value)))

(defun %lower (value)
  (string-downcase (if (stringp value) value (symbol-name value))))

(defun %alpha-p (character)
  (let ((code (char-code character)))
    (or (<= 65 code 90) (<= 97 code 122) (> code 127))))

(defun %dedupe (list &optional (test #'equal))
  (let ((result nil))
    (dolist (item list (nreverse result))
      (unless (member item result :test test) (push item result)))))

(defun %dedupe-by-last (list key &optional (test #'equal))
  (let ((result nil))
    (dolist (item (reverse list) result)
      (unless (member (funcall key item) result :test test :key key)
        (push item result)))))

(defun %in-range-p (value low high)
  (and (integerp value) (<= low value high)))

(defun %ui-color (value &optional nullable)
  (cond ((and nullable (null value)) nil)
        ((%in-range-p value 0 4294967295) value)
        ((and (stringp value) (member (length value) '(4 7 9))
              (char= (char value 0) #\#)
              (every (lambda (c) (digit-char-p c 16)) (subseq value 1)))
         (let ((number (parse-integer value :start 1 :radix 16)))
           (case (length value)
             (4 (logior (ash (* 17 (ldb (byte 4 8) number)) 24)
                        (ash (* 17 (ldb (byte 4 4) number)) 16)
                        (ash (* 17 (ldb (byte 4 0) number)) 8) 255))
             (7 (logior (ash number 8) 255))
             (9 number))))
        (t (error "UI color must be RRGGBBAA or #rgb/#rrggbb/#rrggbbaa."))))

(defun %alist-set (key value alist &optional (test #'equal))
  (let ((entry (assoc key alist :test test)))
    (if entry
        (progn (setf (cdr entry) value) alist)
        (append alist (list (cons key value))))))

(defun %alist-remove (key alist &optional (test #'equal))
  (remove key alist :test test :key #'car))

(defun %plist-remove (key plist)
  (loop for (item value) on plist by #'cddr
        unless (equal item key) append (list item value)))

(defun %error-text (thunk)
  "THUNK's error message, or NIL when it returned."
  (let ((message nil))
    (%catch thunk (lambda (text) (setf message text)))
    message))
