// Copyright (c) 2021, 2023 Tim van der Molen <tim@kariliq.nl>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

package signal

import (
	"errors"
	"sort"
	"strings"
)

type mentionJSON struct {
	Start  int    `json:"start"`
	Length int    `json:"length"`
	UUID   string `json:"mentionUuid"`
}

type Mention struct {
	Start     int
	Length    int
	Recipient *Recipient
}

func (c *Context) parseMentionJSON(body *MessageBody, jmnts []mentionJSON) error {
	for _, jmnt := range jmnts {
		rpt, err := c.recipientFromUUID(jmnt.UUID)
		if err != nil {
			return err
		}
		if rpt == nil {
			warn("cannot find mention recipient for UUID %q", jmnt.UUID)
		}

		mnt := Mention{
			Start:     jmnt.Start,
			Length:    jmnt.Length,
			Recipient: rpt,
		}
		body.Mentions = append(body.Mentions, mnt)
	}

	return nil
}

func (b *MessageBody) insertMentions() error {
	if len(b.Mentions) == 0 {
		return nil
	}

	sort.Slice(b.Mentions, func(i, j int) bool { return b.Mentions[i].Start < b.Mentions[j].Start })

	var runes = []rune(b.Text)
	var text strings.Builder
	var off int

	for i := range b.Mentions {
		mnt := &b.Mentions[i]

		if mnt.Start < off || mnt.Length < 0 || mnt.Start+mnt.Length > len(runes) {
			return errors.New("invalid mention")
		}

		// Copy text preceding mention
		text.WriteString(string(runes[off:mnt.Start]))
		off = mnt.Start + mnt.Length

		repl := "@" + mnt.Recipient.DisplayName()

		// Update mention. Note: the original start and length values
		// were character counts, but the updated values are byte
		// counts.
		mnt.Start = text.Len()
		mnt.Length = len(repl)

		text.WriteString(repl)
	}

	// Copy text succeeding last mention
	text.WriteString(string(runes[off:]))
	b.Text = text.String()

	return nil
}
