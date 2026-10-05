/* Synthetic documents: no recovered game content. */
#define DOC(n) {"Document " #n, "http://example.test/" #n ".pdf", 21, {128}}
static const print_document_t print_documents[] = {
  DOC(1),DOC(2),DOC(3),DOC(4),DOC(5),DOC(6),DOC(7),DOC(8),
  DOC(9),DOC(10),DOC(11),DOC(12),DOC(13),DOC(14),DOC(15),DOC(16)
};
#undef DOC
