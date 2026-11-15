package main

import (
	"net/http"
	"testing"
)

func TestPDReceiveRestoreEndpointRejectsUnownedAndParameterizedRequests(t *testing.T) {
	n := newTestNode(t, "secret")
	api := NewAPI(n)
	for _, test := range []struct {
		path, token, body string
		code              int
	}{
		{"/v1/aetherpd/receive/restore", "wrong", "", http.StatusUnauthorized},
		{"/v1/aetherpd/receive/restore?slice=0", "secret", "", http.StatusBadRequest},
		{"/v1/aetherpd/receive/restore", "secret", "{}", http.StatusBadRequest},
		{"/v1/aetherpd/receive/restore", "secret", "", http.StatusConflict},
	} {
		if got := call(t, api, "POST", test.path, "192.168.50.20:4000", test.token, test.body); got.Code != test.code {
			t.Fatal(test.path, got.Code, got.Body.String())
		}
	}
}
