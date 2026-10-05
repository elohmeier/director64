#let doc = json(sys.inputs.at("document"))
#set document(title: doc.title, author: "Bavaria Film Multimedia", date: none)
#set page(
  paper: "a4",
  margin: (left: 60pt, top: 50pt, right: 50pt, bottom: 40pt),
  footer: align(right, text(size: 9pt)[© Bavaria Film Multimedia]),
)
#set text(font: "Liberation Sans", size: 12pt, lang: "de", hyphenate: false)
#set par(leading: 4pt, spacing: 0pt)
#place(top + right, image(doc.logo, width: doc.logo_width * 1pt))
#v(58pt)
#block(above: 0pt, below: 14pt, text(size: 18pt, weight: "bold", doc.title))
#for paragraph in doc.body.split("\r\r") {
  block(above: 0pt, below: 12pt, breakable: false)[
    #for (i, line) in paragraph.split("\r").enumerate() {
      if i > 0 { linebreak() }
      line
    }
  ]
}
#if doc.illustration != none {
  block(above: 8pt, breakable: false)[
    #h(doc.illustration_x * 1pt)
    #image(doc.illustration, width: 451pt, height: 356pt)
  ]
}
