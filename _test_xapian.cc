#include <xapian.h>
#include <stdio.h>
#include <string>
int main() {
    Xapian::WritableDatabase db = Xapian::WritableDatabase("_test_xapian2",
        Xapian::DB_CREATE_OR_OVERWRITE);
    Xapian::TermGenerator tg;
    tg.set_stemmer(Xapian::Stem("en"));
    
    const char *docs[] = {"Amsterdam","Rotterdam","Utrecht",NULL};
    for (int i=0; docs[i]; i++) {
        Xapian::Document doc;
        tg.set_document(doc);
        tg.index_text(docs[i]);
        doc.set_data(docs[i]);
        db.add_document(doc);
    }
    db.commit();
    
    Xapian::QueryParser qp;
    qp.set_stemmer(Xapian::Stem("en"));
    qp.set_database(db);
    
    const char *queries[] = {"Amsterdam","Amsterdm","Roterdam","Utrect",NULL};
    for (int i=0; queries[i]; i++) {
        printf("Query: '%s'\n", queries[i]);
        
        Xapian::Query q1 = qp.parse_query(queries[i],
            Xapian::QueryParser::FLAG_DEFAULT);
        Xapian::Enquire e1(db); e1.set_query(q1);
        Xapian::MSet m1 = e1.get_mset(0,10);
        printf("  Term: %u results\n", m1.size());
        for (Xapian::MSetIterator it = m1.begin(); it != m1.end(); ++it)
            printf("    [%u] %s\n", it.get_rank(), it.get_document().get_data().c_str());
        
        Xapian::Query q2 = qp.parse_query(queries[i],
            Xapian::QueryParser::FLAG_WILDCARD |
            Xapian::QueryParser::FLAG_SPELLING_CORRECTION);
        Xapian::Enquire e2(db); e2.set_query(q2);
        Xapian::MSet m2 = e2.get_mset(0,10);
        printf("  Spell: %u results\n", m2.size());
        for (Xapian::MSetIterator it = m2.begin(); it != m2.end(); ++it)
            printf("    [%u] %s\n", it.get_rank(), it.get_document().get_data().c_str());
        
        std::string corr = qp.get_corrected_query_string();
        printf("  Corrected: '%s'\n\n", corr.empty() ? "(none)" : corr.c_str());
    }
    db.close();
    return 0;
}
