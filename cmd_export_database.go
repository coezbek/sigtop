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

package main

import (
	"log"
	"os"
	"path/filepath"

	"github.com/tbvdm/sigtop/getopt"
	"github.com/tbvdm/sigtop/signal"
	"github.com/tbvdm/sigtop/util"
)

var cmdExportDatabaseEntry = cmdEntry{
	name:  "export-database",
	alias: "db",
	usage: "[-d signal-directory] file",
	exec:  cmdExportDatabase,
}

func cmdExportDatabase(args []string) cmdStatus {
	getopt.ParseArgs("d:", args)

	var dArg getopt.Arg
	for getopt.Next() {
		switch opt := getopt.Option(); opt {
		case 'd':
			dArg = getopt.OptionArg()
		}
	}

	if err := getopt.Err(); err != nil {
		log.Fatal(err)
	}

	args = getopt.Args()
	if len(args) != 1 {
		return cmdUsage
	}

	dbFile := args[0]

	var signalDir string
	if dArg.Set() {
		signalDir = dArg.String()
	} else {
		var err error
		signalDir, err = signal.DesktopDir()
		if err != nil {
			log.Fatal(err)
		}
	}

	if err := unveilSignalDir(signalDir); err != nil {
		log.Fatal(err)
	}

	// For the export database and its temporary files
	if err := util.Unveil(filepath.Dir(dbFile), "rwc"); err != nil {
		log.Fatal(err)
	}

	// For SQLite/SQLCipher
	if err := util.Unveil("/dev/urandom", "r"); err != nil {
		log.Fatal(err)
	}

	if err := util.Pledge("stdio rpath wpath cpath flock", ""); err != nil {
		log.Fatal(err)
	}

	// SQLite/SQLCipher unconditionally overwrites existing files, so fail
	// here if the export database already exists
	f, err := os.OpenFile(dbFile, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0666)
	if err != nil {
		log.Fatal(err)
	}
	f.Close()

	ctx, err := signal.Open(signalDir)
	if err != nil {
		log.Fatal(err)
	}
	defer ctx.Close()

	if err = ctx.WriteDatabase(dbFile); err != nil {
		log.Print(err)
		return cmdError
	}

	return cmdOK
}
