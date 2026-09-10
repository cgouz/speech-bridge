package httpapi

import (
	"encoding/json"
	"net/http"
)

// errorBody is the stable error shape (see docs/api.md).
type errorBody struct {
	Error struct {
		Code      string `json:"code"`
		Message   string `json:"message"`
		Stage     string `json:"stage,omitempty"`
		RequestID string `json:"request_id,omitempty"`
	} `json:"error"`
}

func writeError(w http.ResponseWriter, status int, code, msg, stage, rid string) {
	var b errorBody
	b.Error.Code = code
	b.Error.Message = msg
	b.Error.Stage = stage
	b.Error.RequestID = rid
	writeJSON(w, status, b)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}
