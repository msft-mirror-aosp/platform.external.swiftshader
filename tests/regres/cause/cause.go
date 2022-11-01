// Copyright 2019 The SwiftShader Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Package cause provides functions for building wrapped errors.
package cause

import (
	"fmt"
	"strings"
)

// Wrap returns a new error wrapping cause with the additional message.
func Wrap(cause error, msg string, args ...interface{}) error {
	s := fmt.Sprintf(msg, args...)
	return fmt.Errorf("%v. Cause: %w", s, cause)
}

// Merge merges all the errors into a single newline delimited error.
func Merge(errs ...error) error {
	if len(errs) == 0 {
		return nil
	}
	strs := make([]string, len(errs))
	for i, err := range errs {
		strs[i] = err.Error()
	}
	return fmt.Errorf("%v", strings.Join(strs, "\n"))
}